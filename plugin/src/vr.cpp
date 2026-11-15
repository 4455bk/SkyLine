#include "vr.h"
#include "state.h"

#ifdef ENABLE_SKYRIM_VR
#	include <d3d11.h>
#	include <dxgi.h>
#	include <openvr.h>

// The image goes up as a D3D11 texture from the game's Present. OpenComposite has no SetOverlayRaw, calling it opens an error box.

namespace
{
	constexpr float  Distance = 2.0f;           // metres in front of the eyes
	constexpr float  Below = 0.35f;             // metres under eye level
	constexpr float  MetresPerPixel = 0.003f;   // at 96 dpi, 16 px text is about 1.4 degrees tall
	constexpr float  MaxMetres = 2.0f;
	constexpr double UpdateSeconds = 0.2;
	constexpr std::uint32_t Plate = 150;        // alpha of the dark plate behind the text

	std::atomic<bool> initialized{ false };
	std::atomic<bool> initHooked{ false };
	std::atomic<bool> failed{ false };

	// Published by the overlay thread.
	std::mutex                 pending;
	std::vector<std::uint32_t> pendingPixels;  // straight RGBA
	int                        pendingWidth = 0;
	int                        pendingHeight = 0;
	float                      pendingScale = 1;
	std::uint64_t              pendingVersion = 0;
	double                     publishedAt = 0;  // overlay thread only

	// Render thread only.
	vr::IVROverlay*            api = nullptr;
	vr::IVRCompositor*         compositor = nullptr;
	vr::VROverlayHandle_t      handle = vr::k_ulOverlayHandleInvalid;
	ID3D11Device*              device = nullptr;
	ID3D11DeviceContext*       context = nullptr;
	ID3D11Texture2D*           texture = nullptr;
	std::vector<std::uint32_t> pixels;
	std::uint64_t              shownVersion = 0;
	int                        width = 0;
	int                        height = 0;
	float                      metres = 0;
	bool                       visible = false;
	bool                       inCall = false;

	const vr::HmdMatrix34_t HeadPose{ { { 1, 0, 0, 0 }, { 0, 1, 0, -Below }, { 0, 0, 1, -Distance } } };

	template <class T>
	void SafeRelease(T*& a_p)
	{
		if (a_p) {
			a_p->Release();
			a_p = nullptr;
		}
	}

	void Fail(std::string_view a_what, int a_error)
	{
		logger::warn("[{:6.1f} s] Headset overlay off, {} failed ({})", Status::Uptime(), a_what, a_error);
		failed = true;
		if (api && handle != vr::k_ulOverlayHandleInvalid) {
			api->DestroyOverlay(handle);
		}
		handle = vr::k_ulOverlayHandleInvalid;
	}

	bool Create()
	{
		const auto module = GetModuleHandleW(L"openvr_api.dll");
		const auto get = module ? reinterpret_cast<void*(*)(const char*, vr::EVRInitError*)>(GetProcAddress(module, "VR_GetGenericInterface")) : nullptr;
		if (!get) {
			logger::warn("[{:6.1f} s] Headset overlay off, no VR_GetGenericInterface in openvr_api.dll", Status::Uptime());
			failed = true;
			return false;
		}
		vr::EVRInitError initError = vr::VRInitError_None;
		api = static_cast<vr::IVROverlay*>(get(vr::IVROverlay_Version, &initError));
		if (!api) {
			logger::warn("[{:6.1f} s] Headset overlay off, no {} ({})", Status::Uptime(), vr::IVROverlay_Version, static_cast<int>(initError));
			failed = true;
			return false;
		}
		compositor = static_cast<vr::IVRCompositor*>(get(vr::IVRCompositor_Version, &initError));
		if (const auto e = api->CreateOverlay("skyline.status", "SkyLine", &handle); e != vr::VROverlayError_None) {
			handle = vr::k_ulOverlayHandleInvalid;
			Fail("CreateOverlay", e);
			return false;
		}
		// Fixed to the head, the world isn't there yet and the user can be facing any way.
		if (const auto e = api->SetOverlayTransformTrackedDeviceRelative(handle, vr::k_unTrackedDeviceIndex_Hmd, &HeadPose); e != vr::VROverlayError_None) {
			Fail("SetOverlayTransformTrackedDeviceRelative", e);
			return false;
		}
		api->SetOverlayFlag(handle, vr::VROverlayFlags_RGSS4X, true);  // cleaner text
		logger::info("[{:6.1f} s] Headset overlay created ({}, {})", Status::Uptime(), vr::IVROverlay_Version,
			compositor ? vr::IVRCompositor_Version : "no compositor interface");
		return true;
	}

	// OpenComposite ignores the head-relative transform and places an overlay by the translation of its absolute one only.
	// So the absolute one is set in front of the headset first. SteamVR goes by the head-relative one set after it.
	void Place()
	{
		if (!compositor) {
			return;
		}
		vr::TrackedDevicePose_t head{};
		if (compositor->GetLastPoses(&head, 1, nullptr, 0) != vr::VRCompositorError_None || !head.bPoseIsValid) {
			return;
		}
		const auto& m = head.mDeviceToAbsoluteTracking.m;
		float       x = -m[0][2];
		float       z = -m[2][2];
		if (const float len = std::sqrt(x * x + z * z); len > 0.01f) {
			x /= len;
			z /= len;
		} else {
			x = 0;
			z = -1;
		}
		const vr::HmdMatrix34_t at{ { { 1, 0, 0, m[0][3] + x * Distance }, { 0, 1, 0, m[1][3] - Below }, { 0, 0, 1, m[2][3] + z * Distance } } };
		api->SetOverlayTransformAbsolute(handle, compositor->GetTrackingSpace(), &at);
		api->SetOverlayTransformTrackedDeviceRelative(handle, vr::k_unTrackedDeviceIndex_Hmd, &HeadPose);
	}

	bool MakeTexture(IDXGISwapChain* a_swapChain, int a_width, int a_height)
	{
		if (texture && a_width == width && a_height == height) {
			return true;
		}
		SafeRelease(texture);
		if (!device) {
			if (FAILED(a_swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) || !device) {
				device = nullptr;
				Fail("getting the game's D3D11 device", 0);
				return false;
			}
			device->GetImmediateContext(&context);
		}
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = static_cast<UINT>(a_width);
		desc.Height = static_cast<UINT>(a_height);
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;  // SteamVR's compositor is another process
		if (const auto hr = device->CreateTexture2D(&desc, nullptr, &texture); FAILED(hr)) {
			texture = nullptr;
			Fail("creating the texture", static_cast<int>(hr));
			return false;
		}
		width = a_width;
		height = a_height;
		logger::info("[{:6.1f} s] Headset overlay texture {}x{}", Status::Uptime(), width, height);
		return true;
	}

	void Update(IDXGISwapChain* a_swapChain)
	{
		int   w = 0;
		int   h = 0;
		float scale = 1;
		{
			std::scoped_lock l{ pending };
			if (pendingVersion == shownVersion) {
				return;
			}
			shownVersion = pendingVersion;
			pixels.swap(pendingPixels);
			w = pendingWidth;
			h = pendingHeight;
			scale = pendingScale;
		}
		if (w <= 0 || h <= 0 || pixels.size() != static_cast<std::size_t>(w) * h) {
			return;
		}
		if (handle == vr::k_ulOverlayHandleInvalid && !Create()) {
			return;
		}
		if (!MakeTexture(a_swapChain, w, h)) {
			return;
		}
		context->UpdateSubresource(texture, 0, nullptr, pixels.data(), static_cast<UINT>(w) * 4, 0);
		context->Flush();
		Place();
		const vr::Texture_t image{ texture, vr::TextureType_DirectX, vr::ColorSpace_Auto };
		if (const auto e = api->SetOverlayTexture(handle, &image); e != vr::VROverlayError_None) {
			Fail("SetOverlayTexture", e);
			return;
		}
		if (const float want = std::min(w * scale * MetresPerPixel, MaxMetres); want != metres) {
			metres = want;
			api->SetOverlayWidthInMeters(handle, metres);
		}
		if (!visible) {
			visible = true;
			api->ShowOverlay(handle);
			logger::info("[{:6.1f} s] Headset overlay showing", Status::Uptime());
		}
	}
}

namespace Vr
{
	void MarkInitialized()
	{
		if (!initialized.exchange(true)) {
			logger::info("[{:6.1f} s] OpenVR is up", Status::Uptime());
		}
	}

	void SetInitHooked(bool a_hooked) { initHooked = a_hooked; }

	bool Due() { return !failed && Status::Uptime() - publishedAt >= UpdateSeconds; }

	void Publish(const std::vector<std::uint32_t>& a_bgra, int a_width, int a_height, int a_canvas, float a_scale)
	{
		publishedAt = Status::Uptime();
		// A fixed width keeps the texture and the compositor's copy of it from being remade whenever the text changes length.
		const int canvas = std::max(a_canvas, a_width);
		const int left = (canvas - a_width) / 2;
		std::vector<std::uint32_t> out(static_cast<std::size_t>(canvas) * a_height, 0);
		for (int y = 0; y < a_height; ++y) {
			for (int x = 0; x < a_width; ++x) {
				const auto p = a_bgra[static_cast<std::size_t>(y) * a_width + x];
				const auto a = p >> 24;
				// The plate keeps the text readable on any background, and the overlay takes straight alpha.
				const auto alpha = a + (255 - a) * Plate / 255;
				const auto straight = [&](int a_shift) { return std::min<std::uint32_t>(((p >> a_shift) & 0xFF) * 255 / std::max<std::uint32_t>(alpha, 1), 255); };
				out[static_cast<std::size_t>(y) * canvas + left + x] = straight(16) | (straight(8) << 8) | (straight(0) << 16) | (alpha << 24);
			}
		}
		std::scoped_lock l{ pending };
		pendingPixels = std::move(out);
		pendingWidth = canvas;
		pendingHeight = a_height;
		pendingScale = a_scale;
		++pendingVersion;
	}

	void OnPresent(IDXGISwapChain* a_swapChain)
	{
		if (!initHooked) {
			MarkInitialized();
		}
		// A call that never came back, OpenComposite shows an error box for what it lacks, must not be made again.
		if (failed || !initialized || inCall) {
			return;
		}
		inCall = true;
		Update(a_swapChain);
		inCall = false;
	}

	void Release()
	{
		failed = true;
		if (!inCall && api && handle != vr::k_ulOverlayHandleInvalid) {
			api->DestroyOverlay(handle);
			handle = vr::k_ulOverlayHandleInvalid;
			logger::info("[{:6.1f} s] Headset overlay closed", Status::Uptime());
		}
		SafeRelease(texture);
		SafeRelease(context);
		SafeRelease(device);
	}
}
#endif
