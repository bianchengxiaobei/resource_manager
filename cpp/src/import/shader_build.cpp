#include "shader_build.hpp"

#include "parcel/core/error.hpp"
#include "parcel/core/path.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace parcel::import::detail {
namespace {

using core::ErrorCode;
using core::MakeError;

struct ProcessResult {
    int exit_code = 0;
    std::string output;
    bool timed_out = false;
};

std::filesystem::path Utf8Path(std::string_view path) {
    return std::filesystem::path(std::u8string(path.begin(), path.end()));
}

core::Result<std::string> AbsolutePath(std::string_view path) {
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(Utf8Path(path), error);
    if (error) {
        return MakeError(ErrorCode::Io, "failed to resolve absolute path: " + error.message())
            .WithPath(std::string(path));
    }
    const std::u8string value = absolute.u8string();
    return core::path::Normalize(
        std::string(reinterpret_cast<const char*>(value.data()), value.size()));
}

std::string OutputTail(std::string_view output, std::size_t limit = 8000) {
    if (output.size() <= limit) {
        return std::string(output);
    }
    return "..." + std::string(output.substr(output.size() - limit));
}

#if defined(_WIN32)

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        result.data(), size);
    return result;
}

std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

std::wstring QuoteWindowsArgument(const std::wstring& argument) {
    if (argument.empty()) {
        return L"\"\"";
    }
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::string WindowsErrorText(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text;
    if (size > 0 && buffer != nullptr) {
        text.assign(buffer, size);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) {
            text.pop_back();
        }
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    if (text.empty()) {
        return "Windows error " + std::to_string(code);
    }
    return WideToUtf8(text);
}

core::Result<ProcessResult> RunProcess(const std::vector<std::string>& arguments,
                                       std::uint32_t timeout_seconds) {
    if (arguments.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "process argument list is empty");
    }
    std::wstring command_line;
    for (const std::string& argument : arguments) {
        const std::wstring wide = Utf8ToWide(argument);
        if (wide.empty() && !argument.empty()) {
            return MakeError(ErrorCode::InvalidArgument, "process argument is not valid UTF-8");
        }
        if (!command_line.empty()) {
            command_line.push_back(L' ');
        }
        command_line += QuoteWindowsArgument(wide);
    }

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (CreatePipe(&read_pipe, &write_pipe, &attributes, 0) == 0) {
        return MakeError(ErrorCode::Io,
                         "failed to create process pipe: " + WindowsErrorText(GetLastError()));
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &attributes, OPEN_EXISTING, 0, nullptr);
    const bool close_input = input != INVALID_HANDLE_VALUE;
    if (!close_input) {
        input = GetStdHandle(STD_INPUT_HANDLE);
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    const DWORD create_error = GetLastError();
    CloseHandle(write_pipe);
    if (close_input) {
        CloseHandle(input);
    }
    if (created == 0) {
        CloseHandle(read_pipe);
        return MakeError(ErrorCode::Io,
                         "failed to launch shader build: " + WindowsErrorText(create_error));
    }

    ProcessResult result;
    std::thread reader([&]() {
        char buffer[4096];
        DWORD read = 0;
        while (ReadFile(read_pipe, buffer, sizeof(buffer), &read, nullptr) != 0 && read > 0) {
            result.output.append(buffer, read);
        }
        CloseHandle(read_pipe);
    });

    const DWORD timeout_ms = timeout_seconds == 0 ? INFINITE : timeout_seconds * 1000U;
    DWORD wait = WaitForSingleObject(process.hProcess, timeout_ms);
    if (wait == WAIT_TIMEOUT) {
        result.timed_out = true;
        TerminateProcess(process.hProcess, 1);
        wait = WaitForSingleObject(process.hProcess, INFINITE);
    }
    if (wait != WAIT_OBJECT_0) {
        const DWORD wait_error = GetLastError();
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
        reader.join();
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return MakeError(ErrorCode::Io,
                         "failed while waiting for shader build: " + WindowsErrorText(wait_error));
    }

    DWORD exit_code = 1;
    if (GetExitCodeProcess(process.hProcess, &exit_code) == 0) {
        const DWORD exit_error = GetLastError();
        reader.join();
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return MakeError(ErrorCode::Io,
                         "failed to read shader build exit code: " + WindowsErrorText(exit_error));
    }
    reader.join();
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    result.exit_code = static_cast<int>(exit_code);
    return result;
}

#else

core::Result<ProcessResult> RunProcess(const std::vector<std::string>& arguments,
                                       std::uint32_t timeout_seconds) {
    if (arguments.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "process argument list is empty");
    }
    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) != 0) {
        return MakeError(ErrorCode::Io, "failed to create process pipe");
    }
    const pid_t child = fork();
    if (child < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return MakeError(ErrorCode::Io, "failed to fork shader build");
    }
    if (child == 0) {
        close(pipe_fds[0]);
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0 || dup2(pipe_fds[1], STDERR_FILENO) < 0) {
            _exit(127);
        }
        close(pipe_fds[1]);
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (const std::string& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(pipe_fds[1]);
    ProcessResult result;
    std::thread reader([&]() {
        char buffer[4096];
        for (;;) {
            const ssize_t count = read(pipe_fds[0], buffer, sizeof(buffer));
            if (count > 0) {
                result.output.append(buffer, static_cast<std::size_t>(count));
            } else if (count == 0) {
                break;
            } else if (errno != EINTR) {
                break;
            }
        }
        close(pipe_fds[0]);
    });

    int status = 0;
    const auto started = std::chrono::steady_clock::now();
    for (;;) {
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            break;
        }
        if (waited < 0 && errno != EINTR) {
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            reader.join();
            return MakeError(ErrorCode::Io, "failed while waiting for shader build");
        }
        if (timeout_seconds > 0 &&
            std::chrono::steady_clock::now() - started >= std::chrono::seconds(timeout_seconds)) {
            result.timed_out = true;
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    reader.join();
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    return result;
}

#endif

} // namespace

core::Result<ShaderBuildResult> RunShaderBuild(const ShaderBuildOptions& options,
                                               std::string_view project_root,
                                               std::string_view expected_manifest) {
    if (options.script.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "shader build script is empty");
    }
    if (options.shaderc.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "shader build shaderc is empty");
    }
    if (options.bgfx_src.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "shader build bgfx source is empty");
    }

    auto root = AbsolutePath(options.root.empty() ? project_root : std::string_view(options.root));
    if (!root) {
        return std::move(root).TakeError();
    }
    auto script = AbsolutePath(options.script);
    if (!script) {
        return std::move(script).TakeError();
    }
    auto shaderc = AbsolutePath(options.shaderc);
    if (!shaderc) {
        return std::move(shaderc).TakeError();
    }
    auto bgfx_src = AbsolutePath(options.bgfx_src);
    if (!bgfx_src) {
        return std::move(bgfx_src).TakeError();
    }

    std::error_code error;
    if (!std::filesystem::is_directory(Utf8Path(root.Value()), error)) {
        return MakeError(ErrorCode::NotFound,
                         "shader build root is not a directory: " + root.Value());
    }
    if (!std::filesystem::is_regular_file(Utf8Path(script.Value()), error)) {
        return MakeError(ErrorCode::NotFound, "shader build script not found: " + script.Value());
    }
    if (!std::filesystem::is_regular_file(Utf8Path(shaderc.Value()), error)) {
        return MakeError(ErrorCode::NotFound, "shaderc not found: " + shaderc.Value());
    }
    if (!std::filesystem::is_directory(Utf8Path(bgfx_src.Value()), error)) {
        return MakeError(ErrorCode::NotFound, "bgfx source not found: " + bgfx_src.Value());
    }

    auto shader_directory = core::path::Join(root.Value(), "shaders");
    if (!shader_directory) {
        return std::move(shader_directory).TakeError();
    }
    auto manifest_path = core::path::Join(shader_directory.Value(), "manifest.json");
    if (!manifest_path) {
        return std::move(manifest_path).TakeError();
    }
    if (!expected_manifest.empty()) {
        auto expected = AbsolutePath(expected_manifest);
        if (!expected) {
            return std::move(expected).TakeError();
        }
        if (core::path::Normalize(expected.Value()) !=
            core::path::Normalize(manifest_path.Value())) {
            return MakeError(ErrorCode::InvalidArgument,
                             "shader build output does not match --shader-manifest: " +
                                 manifest_path.Value());
        }
    }

    std::vector<std::string> arguments;
    if (!options.python_executable.empty()) {
        arguments.push_back(options.python_executable);
    }
    arguments.push_back(script.Value());
    arguments.push_back("--root");
    arguments.push_back(root.Value());
    arguments.push_back("--shaderc");
    arguments.push_back(shaderc.Value());
    arguments.push_back("--bgfx-src");
    arguments.push_back(bgfx_src.Value());
    arguments.push_back("--no-progress");
    arguments.push_back("--timeout");
    arguments.push_back(std::to_string(options.timeout_seconds));
    if (options.jobs > 0) {
        arguments.push_back("--jobs");
        arguments.push_back(std::to_string(options.jobs));
    }
    if (options.force) {
        arguments.push_back("--force");
    }

    auto process = RunProcess(arguments, 0);
    if (!process) {
        return std::move(process).TakeError().WithPath(script.Value());
    }
    if (process.Value().timed_out) {
        return MakeError(ErrorCode::Timeout, "shader build timed out").WithPath(script.Value());
    }
    if (process.Value().exit_code != 0) {
        std::string message =
            "shader build failed with exit code " + std::to_string(process.Value().exit_code);
        if (!process.Value().output.empty()) {
            message += ": " + OutputTail(process.Value().output);
        }
        return MakeError(ErrorCode::Io, std::move(message)).WithPath(script.Value());
    }

    error.clear();
    const bool manifest_exists =
        std::filesystem::is_regular_file(Utf8Path(manifest_path.Value()), error);
    error.clear();
    const std::uintmax_t manifest_size =
        manifest_exists ? std::filesystem::file_size(Utf8Path(manifest_path.Value()), error) : 0;
    if (!manifest_exists || error || manifest_size == 0) {
        return MakeError(ErrorCode::Io,
                         "shader build did not produce manifest: " + manifest_path.Value());
    }
    return ShaderBuildResult{manifest_path.Value()};
}

} // namespace parcel::import::detail
