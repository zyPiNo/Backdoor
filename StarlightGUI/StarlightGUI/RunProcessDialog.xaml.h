#pragma once

#include "RunProcessDialog.g.h"

namespace winrt::StarlightGUI::implementation
{
    struct RunProcessDialog : RunProcessDialogT<RunProcessDialog>
    {
        RunProcessDialog();

        hstring ProcessPath() const { return m_processPath; }
        int Permission() const { return m_permission; }
        bool FullPrivileges() const { return m_fullPrivileges; }

        void OnPrimaryButtonClick(winrt::Microsoft::UI::Xaml::Controls::ContentDialog const& sender,
            winrt::Microsoft::UI::Xaml::Controls::ContentDialogButtonClickEventArgs const& args);

    private:
        hstring m_processPath{ L"" };
        int m_permission{ 0 };
        bool m_fullPrivileges{ false };

    };
}

namespace winrt::StarlightGUI::factory_implementation
{
    struct RunProcessDialog : RunProcessDialogT<RunProcessDialog, implementation::RunProcessDialog>
    {
    };
}