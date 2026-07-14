#pragma once

#include "KCTInfo.g.h"

namespace winrt::StarlightGUI::implementation
{
	struct KCTInfo : KCTInfoT<KCTInfo>
	{
		KCTInfo() = default;

		hstring Name() { return m_name; }
		void Name(hstring const& value) { m_name = value; }

		hstring Address() { return m_address; }
		void Address(hstring const& value) { m_address = value; }

	private:
		hstring m_name{ L"" };
		hstring m_address{ L"" };
	};
}

namespace winrt::StarlightGUI::factory_implementation
{
	struct KCTInfo : KCTInfoT<KCTInfo, implementation::KCTInfo>
	{
	};
}