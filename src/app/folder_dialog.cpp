#include "folder_dialog.h"

#include <memory>

#ifdef _WIN32
#include <windows.h>
#include <shobjidl.h>
#endif

namespace app {

#ifdef _WIN32
namespace {

struct ComRelease {
	void operator()(IUnknown* value) const { value->Release(); }
};
template <class T>
using Com = std::unique_ptr<T, ComRelease>;
struct TaskFree {
	void operator()(wchar_t* value) const { CoTaskMemFree(value); }
};
struct ComApartment {
	bool ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
	~ComApartment() {
		if (ok)
			CoUninitialize();
	}
};

std::wstring wide(const std::string& value) {
	std::wstring result(MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0),
	                    L'\0');
	MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(),
	                    static_cast<int>(result.size()));
	return result;
}

std::string utf8(std::wstring_view value) {
	std::string result(
	    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr),
	    '\0');
	WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(),
	                    static_cast<int>(result.size()), nullptr, nullptr);
	return result;
}

std::optional<std::string> showFolderDialog(HWND owner, const std::wstring& start) {
	const ComApartment apartment;
	if (!apartment.ok)
		return std::nullopt;
	IFileOpenDialog* created = nullptr;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&created))))
		return std::nullopt;
	const Com<IFileOpenDialog> dialog(created);
	FILEOPENDIALOGOPTIONS options{};
	dialog->GetOptions(&options);
	dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
	dialog->SetTitle(L"Open workspace");
	IShellItem* startItem = nullptr;
	if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&startItem)))) {
		const Com<IShellItem> folder(startItem);
		dialog->SetFolder(folder.get());
	}
	if (FAILED(dialog->Show(owner)))
		return std::nullopt; // Includes the user cancelling.
	IShellItem* resultItem = nullptr;
	if (FAILED(dialog->GetResult(&resultItem)))
		return std::nullopt;
	const Com<IShellItem> result(resultItem);
	wchar_t* path = nullptr;
	if (FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)))
		return std::nullopt;
	const std::unique_ptr<wchar_t, TaskFree> owned(path);
	return utf8(owned.get());
}

} // namespace

std::jthread pickFolder(SDL_Window& owner, const std::string& start, FolderChosen done) {
	// The dialog is modal to `owner` but runs its own message loop on this thread, so the app keeps rendering.
	const auto hwnd = static_cast<HWND>(
	    SDL_GetPointerProperty(SDL_GetWindowProperties(&owner), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
	return std::jthread([hwnd, start = wide(start), done = std::move(done)] { done(showFolderDialog(hwnd, start)); });
}
#else
namespace {

void folderSelected(void* userdata, const char* const* files, int) {
	const std::unique_ptr<FolderChosen> done(static_cast<FolderChosen*>(userdata));
	(*done)(files&& files[0] ? std::optional<std::string>(files[0]) : std::nullopt);
}

} // namespace

std::jthread pickFolder(SDL_Window& owner, const std::string& start, FolderChosen done) {
	// SDL owns the asynchronous dialog; its callback takes the callable back.
	auto callback = std::make_unique<FolderChosen>(std::move(done));
	SDL_ShowOpenFolderDialog(folderSelected, callback.release(), &owner, start.c_str(), false);
	return {};
}
#endif

} // namespace app
