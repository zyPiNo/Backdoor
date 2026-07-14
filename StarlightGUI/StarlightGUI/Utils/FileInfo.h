#pragma once

#include "FileInfo.g.h"

namespace winrt::StarlightGUI::implementation
{
	struct FileInfo : FileInfoT<FileInfo>
	{
		FileInfo() = default;

		hstring Name() { return m_name; };
		void Name(hstring const& value) { m_name = value; }

		hstring Path() { return m_path; }
		void Path(hstring const& value) { m_path = value; }

		hstring ModifyTime() { return m_modifyTime; }
		void ModifyTime(hstring const& value) { m_modifyTime = value; }

		ULONG64 ModifyTimeULong() { return m_modifyTimeULong; }
		void ModifyTimeULong(ULONG64 value) { m_modifyTimeULong = value; }

		bool Directory() { return m_directory; }
		void Directory(bool value) { m_directory = value; }

		ULONG Flag() { return m_flag; }
		void Flag(ULONG value) { m_flag = value; }

		hstring Size() { return m_size; }
		void Size(hstring const& value) { m_size = value; }

		ULONG64 SizeULong() { return m_sizeULong; }
		void SizeULong(ULONG64 value) { m_sizeULong = value; }

		ULONG64 MFTID() { return m_mftId; }
		void MFTID(ULONG64 value) { m_mftId = value; }

		winrt::Microsoft::UI::Xaml::Media::ImageSource Icon() { return m_icon; }
		void Icon(winrt::Microsoft::UI::Xaml::Media::ImageSource const& value) { m_icon = value; }

	private:
		hstring m_name{ L"" };
		hstring m_path{ L"" };
		hstring m_modifyTime{ L"" };
		ULONG64 m_modifyTimeULong{ 0 };
		ULONG m_flag{ 0 };
		hstring m_size{ L"" };
		ULONG64 m_sizeULong{ 0 };
		ULONG64 m_mftId{ 0 };
		bool m_directory{ false };
		winrt::Microsoft::UI::Xaml::Media::ImageSource m_icon{ nullptr };
	};
}

namespace winrt::StarlightGUI::factory_implementation
{
	struct FileInfo : FileInfoT<FileInfo, implementation::FileInfo>
	{
	};
}