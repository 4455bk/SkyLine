#include "frame.h"
#include "state.h"

#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_4.h>

// Present hook that blends the overlay's premultiplied box into the back buffer, saving and restoring the state it touches.
// No ID3DDeviceContextState: creating one makes RTSS see a D3D10 device for the rest of the run, and its overlay stays hidden.

namespace
{
	using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
	Present_t originalPresent = nullptr;  // the shared slot's value when hooked
	void**    sharedVtable = nullptr;     // the table the swapchain had, left unchanged
	bool      released = false;         // render thread only
	bool      loggedSwapChain = false;  // render thread only

	std::atomic<bool>          stopped = false;
	std::atomic<std::int64_t>  lastPresentMs = 0;

	// A swapchain wrapper whose buffer 0 isn't the swapchain's size (an upscaler's render-size input) never shows what is drawn there.
	// Then the status goes in at DXGI's own Present, into the image the wrapper presents.
	enum class Route
	{
		Unknown,  // not checked yet
		Game,     // the game's back buffer, as usual
		Waiting,  // DXGI's Present slot being looked up
		Inner,    // DXGI's Present hooked
	};
	Route                  route = Route::Unknown;  // render thread only
	std::atomic<void**>    innerSlot = nullptr;     // set by the lookup thread
	std::atomic<bool>      innerLookupDone = false;
	Present_t              innerPresent = nullptr;
	IDXGISwapChain*        innerChain = nullptr;    // render thread only
	bool                   inGamePresent = false;   // render thread only
	std::atomic<DWORD>     renderThread = 0;        // the thread calling the game's Present
	bool                   innerReached = false;    // render thread only
	int                    framesWithoutInner = 0;  // render thread only

	// The game's swapchain gets its own copy of its vtable with Present replaced. Writing the shared table instead lets an
	// overlay that hooks Present again later (Steam's does) detour our function and call itself in a loop.
	constexpr std::size_t VtableMax = 256;
	void*                 ownVtable[1 + VtableMax]{};  // [0] is the RTTI locator before the table

	// Published by the overlay thread.
	std::mutex                 pending;
	std::vector<std::uint32_t> pendingPixels;
	int                        pendingWidth = 0;
	int                        pendingHeight = 0;
	Frame::Spot                pendingSpot;
	std::uint64_t              pendingVersion = 0;

	// Render thread only.
	ID3D11Device*             device = nullptr;
	ID3D11DeviceContext*      context = nullptr;
	ID3D11On12Device*         on12 = nullptr;  // set when the swapchain is D3D12, its back buffers get wrapped per frame
	ID3D12CommandQueue*       presentQueue = nullptr;  // referenced, the queue D3D11On12 submits on
	ID3D11VertexShader*       vs = nullptr;
	ID3D11PixelShader*        psSrgb = nullptr;    // back buffer stores sRGB-encoded values: write as is
	ID3D11PixelShader*        psLinear = nullptr;  // _SRGB views and scRGB float: convert to linear
	ID3D11BlendState*         blend = nullptr;
	ID3D11RasterizerState*    raster = nullptr;
	ID3D11Texture2D*          texture = nullptr;
	ID3D11ShaderResourceView* view = nullptr;
	std::uint64_t             textureVersion = 0;
	int                       width = 0;
	int                       height = 0;
	Frame::Spot               spot;
	bool                      failed = false;
	POINT                     loggedAt{ -1, -1 };
	bool                      loggedFormat = false;

	constexpr char Shader[] = R"(
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD; };
V vs(uint id : SV_VertexID)
{
	V o;
	o.uv = float2(id & 1, id >> 1);
	o.pos = float4(o.uv.x * 2 - 1, 1 - o.uv.y * 2, 0, 1);
	return o;
}
Texture2D<float4> tex : register(t0);
float4 ps(V i) : SV_Target
{
	uint w, h;
	tex.GetDimensions(w, h);
	float4 c = tex.Load(int3(i.uv * float2(w, h), 0));
#if LINEAR
	float3 s = c.a > 0 ? c.rgb / c.a : 0;
	s = s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
	c.rgb = s * c.a;
#endif
	return c;
}
)";

	template <class T>
	void SafeRelease(T*& a_p)
	{
		if (a_p) {
			a_p->Release();
			a_p = nullptr;
		}
	}

	void ReleaseAll()
	{
		SafeRelease(view);
		SafeRelease(texture);
		SafeRelease(raster);
		SafeRelease(blend);
		SafeRelease(psLinear);
		SafeRelease(psSrgb);
		SafeRelease(vs);
		if (context && on12) {
			context->Flush();
		}
		SafeRelease(context);
		SafeRelease(on12);
		SafeRelease(presentQueue);
		SafeRelease(device);
	}

	ID3DBlob* Compile(const char* a_entry, const char* a_target, bool a_linear)
	{
		using Compile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
			UINT, UINT, ID3DBlob**, ID3DBlob**);
		static const auto compile = [] {
			const auto dll = LoadLibraryW(L"d3dcompiler_47.dll");
			return dll ? reinterpret_cast<Compile_t>(GetProcAddress(dll, "D3DCompile")) : nullptr;
		}();
		if (!compile) {
			logger::error("d3dcompiler_47.dll not available, status not drawn into frames");
			return nullptr;
		}
		const D3D_SHADER_MACRO defines[] = { { "LINEAR", a_linear ? "1" : "0" }, { nullptr, nullptr } };
		ID3DBlob*              code = nullptr;
		ID3DBlob*              errors = nullptr;
		const auto hr = compile(Shader, sizeof(Shader) - 1, "SkyLine", defines, nullptr, a_entry, a_target, 0, 0, &code,
			&errors);
		if (FAILED(hr)) {
			logger::error("Shader {} failed to compile: {}", a_entry,
				errors ? std::string_view(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "?");
		}
		SafeRelease(errors);
		return code;
	}

	// The queue the D3D12 swapchain presents on, when it doesn't report one: the last direct queue the game's thread submitted to
	// inside the game's Present, which is where a wrapper copies its image into the swapchain. Render thread only.
	ID3D12CommandQueue* submittedQueue = nullptr;  // not referenced, reset each game Present
	bool                loggedQueueChange = false;

	using Execute_t = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
	Execute_t originalExecute = nullptr;

	void STDMETHODCALLTYPE HookedExecute(ID3D12CommandQueue* a_queue, UINT a_count, ID3D12CommandList* const* a_lists)
	{
		if (!stopped && GetCurrentThreadId() == renderThread && inGamePresent && a_queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
			submittedQueue = a_queue;
		}
		TAIL_CALL return originalExecute(a_queue, a_count, a_lists);
	}

	// Hooks ExecuteCommandLists in the D3D12 queue vtable, found from a throwaway queue. It stays a pass-through after startup.
	void HookExecute(ID3D12Device* a_device)
	{
		if (originalExecute) {
			return;
		}
		ID3D12CommandQueue*      dummy = nullptr;
		D3D12_COMMAND_QUEUE_DESC desc{};
		desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (FAILED(a_device->CreateCommandQueue(&desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&dummy)))) {
			logger::warn("[{:6.1f} s] No D3D12 queue for the submit hook, status not drawn into frames", Status::Uptime());
			return;
		}
		auto** slot = &(*reinterpret_cast<void***>(dummy))[10];  // ID3D12CommandQueue::ExecuteCommandLists
		DWORD  old = 0;
		if (VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
			originalExecute = reinterpret_cast<Execute_t>(*slot);
			InterlockedExchangePointer(slot, reinterpret_cast<void*>(&HookedExecute));
			VirtualProtect(slot, sizeof(void*), old, &old);
			logger::info("[{:6.1f} s] D3D12 swapchain reports no queue, ExecuteCommandLists hooked to find the one it presents on", Status::Uptime());
		}
		dummy->Release();
	}

	// A D3D11On12 device on the D3D12 swapchain's queue lets the same drawing code run. False until that queue is known.
	bool InitOn12(IDXGISwapChain* a_swapChain)
	{
		using Create_t = decltype(&D3D11On12CreateDevice);
		const auto          create = reinterpret_cast<Create_t>(GetProcAddress(GetModuleHandleW(L"d3d11.dll"), "D3D11On12CreateDevice"));
		ID3D12CommandQueue* queue = nullptr;
		ID3D12Device*       device12 = nullptr;
		if (SUCCEEDED(a_swapChain->GetDevice(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue)))) {
			queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device12));
		} else if (SUCCEEDED(a_swapChain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device12)))) {
			HookExecute(device12);
			if (submittedQueue) {
				queue = submittedQueue;
				queue->AddRef();
			}
		}
		bool ok = create && queue && device12;
		if (ok) {
			IUnknown*  queues[] = { queue };
			const auto hr = create(device12, 0, nullptr, 0, queues, 1, 0, &device, &context, nullptr);
			if (FAILED(hr)) {
				logger::warn("D3D11On12CreateDevice failed {:08X}", static_cast<std::uint32_t>(hr));
			}
			ok = SUCCEEDED(hr) && SUCCEEDED(device->QueryInterface(__uuidof(ID3D11On12Device), reinterpret_cast<void**>(&on12)));
		}
		SafeRelease(device12);
		if (ok) {
			presentQueue = queue;
			logger::info("[{:6.1f} s] Swapchain is D3D12, drawing through D3D11On12", Status::Uptime());
		} else {
			SafeRelease(queue);
		}
		return ok;
	}

	// D3D12 without a known queue yet: try again next frame instead of giving up.
	bool waitingForQueue = false;

	bool Init(IDXGISwapChain* a_swapChain)
	{
		waitingForQueue = false;
		if (SUCCEEDED(a_swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device)))) {
			device->GetImmediateContext(&context);
		} else if (!InitOn12(a_swapChain)) {
			ID3D12Device* device12 = nullptr;
			if (originalExecute && SUCCEEDED(a_swapChain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device12)))) {
				device12->Release();
				waitingForQueue = true;
			} else {
				logger::warn("Swapchain has neither a D3D11 device nor a D3D12 queue, status not drawn into frames");
			}
			ReleaseAll();
			return false;
		}
		const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
		auto* vsCode = Compile("vs", "vs_4_0", false);
		auto* psCode = Compile("ps", "ps_4_0", false);
		auto* psLinCode = Compile("ps", "ps_4_0", true);
		bool  ok = vsCode && psCode && psLinCode &&
		          SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs)) &&
		          SUCCEEDED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &psSrgb)) &&
		          SUCCEEDED(device->CreatePixelShader(psLinCode->GetBufferPointer(), psLinCode->GetBufferSize(), nullptr, &psLinear));
		SafeRelease(vsCode);
		SafeRelease(psCode);
		SafeRelease(psLinCode);

		D3D11_BLEND_DESC bd{};
		auto&            rt = bd.RenderTarget[0];
		rt.BlendEnable = TRUE;
		rt.SrcBlend = D3D11_BLEND_ONE;  // premultiplied
		rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		rt.BlendOp = D3D11_BLEND_OP_ADD;
		rt.SrcBlendAlpha = D3D11_BLEND_ZERO;  // keep the back buffer's alpha
		rt.DestBlendAlpha = D3D11_BLEND_ONE;
		rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		D3D11_RASTERIZER_DESC rd{};
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_NONE;
		rd.DepthClipEnable = TRUE;
		ok = ok && SUCCEEDED(device->CreateBlendState(&bd, &blend)) && SUCCEEDED(device->CreateRasterizerState(&rd, &raster));
		if (!ok) {
			logger::error("Shader setup failed, status not drawn into frames");
		} else {
			logger::info("[{:6.1f} s] Frame drawing ready (feature level {:x})", Status::Uptime(), static_cast<int>(level));
		}
		return ok;
	}

	// The render target view format for the back buffer, and whether the shader must write linear values.
	bool ViewFormat(DXGI_FORMAT a_format, DXGI_FORMAT& a_view, bool& a_linear)
	{
		a_view = a_format;
		a_linear = false;
		switch (a_format) {
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
			a_view = DXGI_FORMAT_B8G8R8A8_UNORM;
			return true;
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			a_view = DXGI_FORMAT_R8G8B8A8_UNORM;
			return true;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS:
			a_view = DXGI_FORMAT_R10G10B10A2_UNORM;
			return true;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			a_view = DXGI_FORMAT_R16G16B16A16_FLOAT;
			a_linear = true;
			return true;
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8X8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R10G10B10A2_UNORM:
			return true;
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:  // scRGB: linear, 1.0 = SDR white
			a_linear = true;
			return true;
		default:
			return false;
		}
	}

	// Everything the draw changes. The game tracks its own bindings, so they must be back as they were before Present.
	struct SavedState
	{
		D3D11_PRIMITIVE_TOPOLOGY  topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		ID3D11InputLayout*        layout = nullptr;
		ID3D11VertexShader*       vertex = nullptr;
		ID3D11HullShader*         hull = nullptr;
		ID3D11DomainShader*       domain = nullptr;
		ID3D11GeometryShader*     geometry = nullptr;
		ID3D11PixelShader*        pixel = nullptr;
		ID3D11ShaderResourceView* resource = nullptr;
		ID3D11RasterizerState*    rasterizer = nullptr;
		UINT                      viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		D3D11_VIEWPORT            viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		ID3D11BlendState*         blending = nullptr;
		FLOAT                     blendFactor[4]{};
		UINT                      sampleMask = 0;
		ID3D11DepthStencilState*  depth = nullptr;
		UINT                      stencilRef = 0;
		ID3D11RenderTargetView*   targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView*   depthTarget = nullptr;

		void Save()
		{
			context->IAGetPrimitiveTopology(&topology);
			context->IAGetInputLayout(&layout);
			context->VSGetShader(&vertex, nullptr, nullptr);
			context->HSGetShader(&hull, nullptr, nullptr);
			context->DSGetShader(&domain, nullptr, nullptr);
			context->GSGetShader(&geometry, nullptr, nullptr);
			context->PSGetShader(&pixel, nullptr, nullptr);
			context->PSGetShaderResources(0, 1, &resource);
			context->RSGetState(&rasterizer);
			context->RSGetViewports(&viewportCount, viewports);
			context->OMGetBlendState(&blending, blendFactor, &sampleMask);
			context->OMGetDepthStencilState(&depth, &stencilRef);
			context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depthTarget);
		}

		// Also drops the references Save took.
		void Restore()
		{
			context->IASetPrimitiveTopology(topology);
			context->IASetInputLayout(layout);
			context->VSSetShader(vertex, nullptr, 0);
			context->HSSetShader(hull, nullptr, 0);
			context->DSSetShader(domain, nullptr, 0);
			context->GSSetShader(geometry, nullptr, 0);
			context->PSSetShader(pixel, nullptr, 0);
			context->PSSetShaderResources(0, 1, &resource);
			context->RSSetState(rasterizer);
			context->RSSetViewports(viewportCount, viewports);
			context->OMSetBlendState(blending, blendFactor, sampleMask);
			context->OMSetDepthStencilState(depth, stencilRef);
			context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, depthTarget);
			SafeRelease(layout);
			SafeRelease(vertex);
			SafeRelease(hull);
			SafeRelease(domain);
			SafeRelease(geometry);
			SafeRelease(pixel);
			SafeRelease(resource);
			SafeRelease(rasterizer);
			SafeRelease(blending);
			SafeRelease(depth);
			for (auto*& target : targets) {
				SafeRelease(target);
			}
			SafeRelease(depthTarget);
		}
	};

	// The current back buffer as a D3D11 texture. A D3D12 one is wrapped and must go back through ReleaseBack.
	ID3D11Texture2D* AcquireBack(IDXGISwapChain* a_swapChain)
	{
		ID3D11Texture2D* back = nullptr;
		if (!on12) {
			a_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
			return back;
		}
		IDXGISwapChain3* chain3 = nullptr;
		ID3D12Resource*  resource = nullptr;
		if (SUCCEEDED(a_swapChain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&chain3))) &&
			SUCCEEDED(chain3->GetBuffer(chain3->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), reinterpret_cast<void**>(&resource)))) {
			const D3D11_RESOURCE_FLAGS flags{ D3D11_BIND_RENDER_TARGET, 0, 0, 0 };
			if (SUCCEEDED(on12->CreateWrappedResource(resource, &flags, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT,
					__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) {
				ID3D11Resource* wrapped = back;
				on12->AcquireWrappedResources(&wrapped, 1);
			}
		}
		SafeRelease(resource);
		SafeRelease(chain3);
		return back;
	}

	// Hands a wrapped D3D12 buffer back in the present state and submits the draw before the swapchain presents.
	void ReleaseBack(ID3D11Texture2D*& a_back)
	{
		if (a_back && on12) {
			ID3D11Resource* wrapped = a_back;
			on12->ReleaseWrappedResources(&wrapped, 1);
			context->Flush();
		}
		SafeRelease(a_back);
	}

	void Draw(IDXGISwapChain* a_swapChain)
	{
		if (failed) {
			return;
		}
		if (!device && !Init(a_swapChain)) {
			failed = !waitingForQueue;
			ReleaseAll();
			return;
		}

		// New pixels: rebuild the texture (never wait on the overlay thread).
		if (std::unique_lock l{ pending, std::try_to_lock }; l && pendingWidth && pendingVersion != textureVersion) {
			D3D11_TEXTURE2D_DESC td{};
			td.Width = pendingWidth;
			td.Height = pendingHeight;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_IMMUTABLE;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			const D3D11_SUBRESOURCE_DATA init{ pendingPixels.data(), static_cast<UINT>(pendingWidth * 4), 0 };
			ID3D11Texture2D*             t = nullptr;
			ID3D11ShaderResourceView*    v = nullptr;
			if (SUCCEEDED(device->CreateTexture2D(&td, &init, &t)) && SUCCEEDED(device->CreateShaderResourceView(t, nullptr, &v))) {
				SafeRelease(view);
				SafeRelease(texture);
				texture = t;
				view = v;
				width = pendingWidth;
				height = pendingHeight;
				spot = pendingSpot;
			} else {
				SafeRelease(t);
			}
			textureVersion = pendingVersion;
		}
		if (!view) {
			return;
		}

		ID3D11Texture2D* back = AcquireBack(a_swapChain);
		if (!back) {
			return;
		}
		D3D11_TEXTURE2D_DESC bd{};
		back->GetDesc(&bd);
		DXGI_FORMAT format;
		bool        linear;
		if (!ViewFormat(bd.Format, format, linear)) {
			if (!loggedFormat) {
				loggedFormat = true;
				logger::warn("Back buffer format {} not handled, status not drawn into frames", static_cast<int>(bd.Format));
			}
			ReleaseBack(back);
			return;
		}
		// The RTV is made per frame and released before Present, a held back buffer reference would make ResizeBuffers fail.
		D3D11_RENDER_TARGET_VIEW_DESC rd{};
		rd.Format = format;
		rd.ViewDimension = bd.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
		ID3D11RenderTargetView* rtv = nullptr;
		const auto at = Frame::Place({ static_cast<LONG>(bd.Width), static_cast<LONG>(bd.Height) }, { width, height }, spot);
		if (at.x >= 0 && at.y >= 0 && at.x + width <= static_cast<int>(bd.Width) && at.y + height <= static_cast<int>(bd.Height) &&
			SUCCEEDED(device->CreateRenderTargetView(back, &rd, &rtv))) {
			if (at.x != loggedAt.x || at.y != loggedAt.y) {
				loggedAt = at;
				logger::info("[{:6.1f} s] Status drawn into frames at {},{} ({}x{} in {}x{})", Status::Uptime(), at.x, at.y, width,
					height, bd.Width, bd.Height);
			}
			SavedState game;
			game.Save();
			const D3D11_VIEWPORT vp{ static_cast<float>(at.x), static_cast<float>(at.y), static_cast<float>(width),
				static_cast<float>(height), 0.0f, 1.0f };
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
			context->IASetInputLayout(nullptr);
			context->VSSetShader(vs, nullptr, 0);
			context->HSSetShader(nullptr, nullptr, 0);
			context->DSSetShader(nullptr, nullptr, 0);
			context->GSSetShader(nullptr, nullptr, 0);
			context->PSSetShader(linear ? psLinear : psSrgb, nullptr, 0);
			context->PSSetShaderResources(0, 1, &view);
			context->RSSetState(raster);
			context->RSSetViewports(1, &vp);
			context->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
			context->OMSetDepthStencilState(nullptr, 0);
			context->OMSetRenderTargets(1, &rtv, nullptr);
			context->Draw(4, 0);
			game.Restore();
			rtv->Release();
		}
		ReleaseBack(back);
	}

	// An exclusive fullscreen or D3D12 output can hide the child window, this tells which one a report is about.
	// Runs on the game's own thread, since a report's log stopped at these calls when the overlay thread made them.
	void LogSwapChain(IDXGISwapChain* a_swapChain)
	{
		logger::info("[{:6.1f} s] First Present on thread {} (main thread {})", Status::Uptime(), GetCurrentThreadId(), Status::mainThread.load());
		DXGI_SWAP_CHAIN_DESC desc{};
		BOOL                 fullscreen = FALSE;
		a_swapChain->GetDesc(&desc);
		a_swapChain->GetFullscreenState(&fullscreen, nullptr);
		logger::info("[{:6.1f} s] Swapchain: swap effect {}, fullscreen {}, d3d12.dll {}", Status::Uptime(),
			static_cast<int>(desc.SwapEffect), fullscreen != FALSE, GetModuleHandleW(L"d3d12.dll") != nullptr ? "loaded" : "not loaded");
	}

	// The Steam overlay calls Present again through the slot from inside its own hook, nested calls only pass through.
	thread_local bool insideInner = false;

	// Kept out of InnerPresent so that hook stays a plain jump to DXGI's Present after startup.
	__declspec(noinline) HRESULT OnInnerPresent(IDXGISwapChain* a_swapChain, UINT a_sync, UINT a_flags)
	{
		insideInner = true;
		// Only inside the game's Present, so other swapchains and the wrapper's own extra presents are left alone.
		if (inGamePresent && !(a_flags & DXGI_PRESENT_TEST)) {
			if (!innerChain) {
				innerChain = a_swapChain;
				logger::info("[{:6.1f} s] Drawing into frames at DXGI's Present, after the swapchain wrapper", Status::Uptime());
			}
			if (a_swapChain == innerChain) {
				innerReached = true;
				if (presentQueue && submittedQueue && submittedQueue != presentQueue && !loggedQueueChange) {
					loggedQueueChange = true;
					logger::warn("[{:6.1f} s] The last queue submitted to before Present changed, drawing may be out of order", Status::Uptime());
				}
				Draw(a_swapChain);
			}
		}
		const auto hr = innerPresent(a_swapChain, a_sync, a_flags);
		insideInner = false;
		return hr;
	}

	HRESULT STDMETHODCALLTYPE InnerPresent(IDXGISwapChain* a_swapChain, UINT a_sync, UINT a_flags)
	{
		if (!stopped && !insideInner) {
			return OnInnerPresent(a_swapChain, a_sync, a_flags);
		}
		TAIL_CALL return innerPresent(a_swapChain, a_sync, a_flags);
	}

	// Finds Present's slot in DXGI's swapchain vtable from a throwaway swapchain made with the system d3d11.dll. It can take seconds, so it runs apart.
	void LookUpInner()
	{
		wchar_t system[MAX_PATH]{};
		GetSystemDirectoryW(system, MAX_PATH);
		const auto d3d11 = LoadLibraryW((std::filesystem::path(system) / L"d3d11.dll").c_str());
		const auto create = d3d11 ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain")) : nullptr;
		const auto window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 8, 8, nullptr, nullptr, nullptr, nullptr);
		IDXGISwapChain*      chain = nullptr;
		ID3D11Device*        dummy = nullptr;
		ID3D11DeviceContext* dummyContext = nullptr;
		if (create && window) {
			DXGI_SWAP_CHAIN_DESC desc{};
			desc.BufferCount = 1;
			desc.BufferDesc.Width = 8;
			desc.BufferDesc.Height = 8;
			desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			desc.OutputWindow = window;
			desc.SampleDesc.Count = 1;
			desc.Windowed = TRUE;
			desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
			create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &chain, &dummy, nullptr, &dummyContext);
		}
		if (chain) {
			innerSlot = &(*reinterpret_cast<void***>(chain))[8];
		}
		SafeRelease(chain);
		SafeRelease(dummyContext);
		SafeRelease(dummy);
		if (window) {
			DestroyWindow(window);
		}
		innerLookupDone = true;
	}

	// Compares buffer 0 with the swapchain's own size, once, on the game's thread.
	void CheckRoute(IDXGISwapChain* a_swapChain)
	{
		route = Route::Game;
		DXGI_SWAP_CHAIN_DESC desc{};
		ID3D11Texture2D*     back = nullptr;
		if (FAILED(a_swapChain->GetDesc(&desc)) ||
			FAILED(a_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) {
			return;
		}
		D3D11_TEXTURE2D_DESC bd{};
		back->GetDesc(&bd);
		back->Release();
		if (bd.Width == desc.BufferDesc.Width && bd.Height == desc.BufferDesc.Height) {
			return;
		}
		logger::info("[{:6.1f} s] Back buffer {}x{} isn't the swapchain's {}x{}, a wrapper presents another image, looking up DXGI's Present",
			Status::Uptime(), bd.Width, bd.Height, desc.BufferDesc.Width, desc.BufferDesc.Height);
		route = Route::Waiting;
		std::thread(LookUpInner).detach();
	}

	// The slot write, on the game's thread, once the lookup is done.
	void InstallInner()
	{
		auto** slot = innerSlot.load();
		DWORD  old = 0;
		if (!slot || !VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
			logger::warn("[{:6.1f} s] DXGI's Present not found, status drawn into the wrapper's buffer", Status::Uptime());
			route = Route::Game;
			return;
		}
		innerPresent = reinterpret_cast<Present_t>(*slot);
		InterlockedExchangePointer(slot, reinterpret_cast<void*>(&InnerPresent));
		VirtualProtect(slot, sizeof(void*), old, &old);
		route = Route::Inner;
		logger::info("[{:6.1f} s] DXGI Present hooked", Status::Uptime());
	}

	// Kept out of HookedPresent so the hook stays a plain jump to the next Present. True when DXGI's Present does the drawing.
	__declspec(noinline) bool OnPresent(IDXGISwapChain* a_swapChain)
	{
		if (!stopped) {
			if (!loggedSwapChain) {
				loggedSwapChain = true;
				LogSwapChain(a_swapChain);
			}
			lastPresentMs = static_cast<std::int64_t>(Status::Uptime() * 1000.0);
			if (route == Route::Unknown) {
				CheckRoute(a_swapChain);
			}
			if (route == Route::Waiting && innerLookupDone) {
				InstallInner();
			}
			if (route == Route::Game) {
				Draw(a_swapChain);
			}
			return route == Route::Inner;
		}
		// The swapchain keeps our vtable: a mod may have detoured this function's code, Community Shaders does, and putting the old one back would cut it out.
		released = true;
		ReleaseAll();
		logger::info("[{:6.1f} s] Present hook stays as a pass-through", Status::Uptime());
		return false;
	}

	// The game's Present with InnerPresent drawing inside it, startup only.
	__declspec(noinline) HRESULT PresentThroughInner(IDXGISwapChain* a_swapChain, UINT a_sync, UINT a_flags, Present_t a_next)
	{
		renderThread = GetCurrentThreadId();
		inGamePresent = true;
		innerReached = false;
		submittedQueue = nullptr;
		const auto hr = a_next(a_swapChain, a_sync, a_flags);
		inGamePresent = false;
		framesWithoutInner = innerReached ? 0 : framesWithoutInner + 1;
		if (framesWithoutInner >= 3) {
			// The slot stays hooked, it only passes through from here.
			logger::warn("[{:6.1f} s] DXGI's Present not reached for 3 frames, status drawn into the wrapper's buffer", Status::Uptime());
			route = Route::Game;
			ReleaseAll();
			failed = false;
			textureVersion = 0;
		}
		return hr;
	}

	HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* a_swapChain, UINT a_sync, UINT a_flags)
	{
		// Follows the shared slot, so a mod that hooks it after us still runs for the game's swapchain.
		auto next = reinterpret_cast<Present_t>(sharedVtable[8]);
		if (next == &HookedPresent) {
			next = originalPresent;
		}
		if (!released && !(a_flags & DXGI_PRESENT_TEST) && OnPresent(a_swapChain)) {
			return PresentThroughInner(a_swapChain, a_sync, a_flags, next);
		}
		// After startup the next Present runs with no frame of ours below it, so hitch tools don't blame SkyLine for its waits.
		TAIL_CALL return next(a_swapChain, a_sync, a_flags);
	}
}

namespace Frame
{
	bool TryHook()
	{
		const auto data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
		const auto swapChain = data ? reinterpret_cast<IDXGISwapChain*>(data->renderWindows[0].swapChain) : nullptr;
		if (!swapChain) {
			return false;
		}
		auto** vtable = *reinterpret_cast<void***>(swapChain);
		// Copy up to the first entry that isn't code or VtableMax, whichever comes first. The table's length isn't known and
		// tables can follow each other with nothing between them: copying past the end is harmless, stopping short isn't.
		const auto readable = [](const void* a_p) {
			MEMORY_BASIC_INFORMATION m{};
			return VirtualQuery(a_p, &m, sizeof(m)) && m.State == MEM_COMMIT && !(m.Protect & (PAGE_NOACCESS | PAGE_GUARD));
		};
		const auto code = [](const void* a_p) {
			MEMORY_BASIC_INFORMATION m{};
			return VirtualQuery(a_p, &m, sizeof(m)) && m.State == MEM_COMMIT &&
			       (m.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
		};
		std::size_t count = 0;
		while (count < VtableMax && readable(&vtable[count]) && code(vtable[count])) {
			++count;
		}
		if (count < 18) {  // IDXGISwapChain's methods
			logger::error("Swapchain vtable not recognised ({} entries), status not drawn into frames", count);
			return true;  // do not retry
		}
		ownVtable[0] = readable(&vtable[-1]) ? vtable[-1] : nullptr;
		std::copy_n(vtable, count, &ownVtable[1]);
		sharedVtable = vtable;
		originalPresent = reinterpret_cast<Present_t>(vtable[8]);  // IDXGISwapChain::Present
		ownVtable[1 + 8] = reinterpret_cast<void*>(&HookedPresent);
		InterlockedExchangePointer(reinterpret_cast<void**>(swapChain), &ownVtable[1]);
		logger::info("[{:6.1f} s] Present hooked on the game swapchain ({} vtable entries)", Status::Uptime(), count);
		return true;
	}

	double LastPresent() { return lastPresentMs / 1000.0; }

	void Publish(std::vector<std::uint32_t> a_bgra, int a_width, int a_height, const Spot& a_spot)
	{
		std::scoped_lock l{ pending };
		pendingPixels = std::move(a_bgra);
		pendingWidth = a_width;
		pendingHeight = a_height;
		pendingSpot = a_spot;
		++pendingVersion;
	}

	void Stop() { stopped = true; }
}
