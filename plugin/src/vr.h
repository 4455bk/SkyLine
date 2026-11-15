#pragma once

#ifdef ENABLE_SKYRIM_VR
struct IDXGISwapChain;

// Shows the status in the headset as an OpenVR overlay, the compositor keeps it up while the game is busy.
namespace Vr
{
	inline constexpr int MaxWidthPx = 560;  // at 96 dpi

	// The game's VR_Init returned, called on the game's thread.
	void MarkInitialized();
	// Whether the game's VR_Init import is hooked. When it isn't, the first frame stands in for it.
	void SetInitHooked(bool a_hooked);

	// True when the overlay should get a new image. Overlay thread only, like Publish.
	bool Due();
	// Takes the box as top-down premultiplied BGRA, centred in an a_canvas wide image. a_scale converts pixels to pixels at 96 dpi.
	void Publish(const std::vector<std::uint32_t>& a_bgra, int a_width, int a_height, int a_canvas, float a_scale);

	// Sends a new image to the headset. Render thread only, like Release.
	void OnPresent(IDXGISwapChain* a_swapChain);
	void Release();
}
#endif
