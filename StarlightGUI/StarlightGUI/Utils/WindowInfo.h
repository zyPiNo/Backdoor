#pragma once

#include "WindowInfo.g.h"

namespace winrt::StarlightGUI::implementation
{
	struct WindowInfo : WindowInfoT<WindowInfo>
	{
		WindowInfo() = default;

		hstring Name() { return m_name; }
		void Name(hstring const& value) { m_name = value; }

		hstring Description() { return m_description; }
		void Description(hstring const& value) { m_description = value; }

		hstring Process() { return m_process; }
		void Process(hstring const& value) { m_process = value; }

		hstring ClassName() { return m_className; }
		void ClassName(hstring const& value) { m_className = value; }

		uint64_t Hwnd() { return m_hwnd; }
		void Hwnd(uint64_t value) { m_hwnd = value; }

		int32_t FromPID() { return m_fromPID; }
		void FromPID(int32_t value) { m_fromPID = value; }

		int32_t WindowStyle() { return m_windowStyle; }
		void WindowStyle(int32_t value) { m_windowStyle = value; }

		int32_t WindowStyleEx() { return m_windowStyleEx; }
		void WindowStyleEx(int32_t value) { m_windowStyleEx = value; }

		int32_t Band() { return m_band; }
		void Band(int32_t value) { m_band = value; }

		winrt::Microsoft::UI::Xaml::Media::ImageSource Icon() { return m_icon; }
		void Icon(winrt::Microsoft::UI::Xaml::Media::ImageSource const& value) { m_icon = value; }

	private:
		hstring m_name{ L"" };
		hstring m_description{ L"" };
		hstring m_process{ L"" };
		hstring m_className{ L"" };
		uint64_t m_hwnd{ 0 };
		int32_t m_fromPID{ 0 };
		int32_t m_windowStyle{ 0 };
		int32_t m_windowStyleEx{ 0 };
		int32_t m_band{ 0 };
		winrt::Microsoft::UI::Xaml::Media::ImageSource m_icon{ nullptr };
	};
}

namespace winrt::StarlightGUI::factory_implementation
{
	struct WindowInfo : WindowInfoT<WindowInfo, implementation::WindowInfo>
	{
	};
}