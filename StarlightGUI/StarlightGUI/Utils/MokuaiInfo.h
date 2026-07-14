#pragma once

#include "MokuaiInfo.g.h"
/*
* 补药问我为什么是Mokuai不是Module，ModuleInfo会报错，我不知道为什么
* @Author Stars
*/
namespace winrt::StarlightGUI::implementation
{
	struct MokuaiInfo : MokuaiInfoT<MokuaiInfo>
	{
		MokuaiInfo() = default;

		hstring Name() { return m_name; }
		void Name(hstring const& value) { m_name = value; }

		hstring Address() { return m_address; }
		void Address(hstring const& value) { m_address = value; }

		hstring Size() { return m_size; }
		void Size(hstring const& value) { m_size = value; }

		hstring Path() { return m_path; }
		void Path(hstring const& value) { m_path = value; }

	private:
		hstring m_name{ L"" };
		hstring m_address{ L"" };
		hstring m_size{ L"" };
		hstring m_path{ L"" };
	};
}

namespace winrt::StarlightGUI::factory_implementation
{
	struct MokuaiInfo : MokuaiInfoT<MokuaiInfo, implementation::MokuaiInfo>
	{
	};
}