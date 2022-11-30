#include <InternalLoader.hpp>
#include <Geode/loader/Log.hpp>
#include <Geode/loader/IPC.hpp>
#include <iostream>
#include <InternalMod.hpp>

USE_GEODE_NAMESPACE();

#ifdef GEODE_IS_WINDOWS

static constexpr auto IPC_BUFFER_SIZE = 512;

void InternalLoader::platformMessageBox(char const* title, std::string const& info) {
    MessageBoxA(nullptr, info.c_str(), title, MB_ICONERROR);
}

void InternalLoader::openPlatformConsole() {
    if (m_platformConsoleOpen) return;
    if (AllocConsole() == 0) return;
    SetConsoleCP(CP_UTF8);
    // redirect console output
    freopen_s(reinterpret_cast<FILE**>(stdout), "CONOUT$", "w", stdout);
    freopen_s(reinterpret_cast<FILE**>(stdin), "CONIN$", "r", stdin);

    m_platformConsoleOpen = true;

    for (auto const& log : Loader::get()->getLogs()) {
        std::cout << log->toString(true) << "\n";
    }
}

void InternalLoader::closePlatformConsole() {
    if (!m_platformConsoleOpen) return;

    fclose(stdin);
    fclose(stdout);
    FreeConsole();

    m_platformConsoleOpen = false;
}

void InternalLoader::postIPCReply(
    void* rawPipeHandle,
    std::string const& replyID,
    nlohmann::json const& data
) {
    auto msgJson = nlohmann::json::object();
    msgJson["reply"] = replyID;
    msgJson["data"] = data;
    auto msg = msgJson.dump();

    DWORD written;
    WriteFile(rawPipeHandle, msg.c_str(), msg.size(), &written, nullptr);

    // log::debug("Sent message {}", msg);
}

void ipcPipeThread(HANDLE pipe) {
    char buffer[IPC_BUFFER_SIZE * sizeof(TCHAR)];
    DWORD read;

    std::optional<std::string> replyID = std::nullopt;

    // log::debug("Waiting for I/O");
    if (ReadFile(pipe, buffer, sizeof(buffer) - 1, &read, nullptr)) {
        buffer[read] = '\0';
        // log::debug("Got message {}", buffer);
        try {
            // parse received message
            auto json = nlohmann::json::parse(buffer);
            if (!json.contains("mod") || !json["mod"].is_string()) {
                log::warn("Received IPC message without 'mod' field");
                goto ipc_done;
            }
            if (!json.contains("message") || !json["message"].is_string()) {
                log::warn("Received IPC message without 'message' field");
                goto ipc_done;
            }
            if (json.contains("reply") && json["reply"].is_string()) {
                replyID = json["reply"];
            }
            nlohmann::json data;
            if (json.contains("data")) {
                data = json["data"];
            }
            // log::debug("Posting IPC event");
            // ! warning: if the event system is ever made asynchronous this will break!
            IPCEvent(pipe, json["mod"], json["message"], replyID, data).post();
        } catch(...) {
            log::warn("Received IPC message that isn't valid JSON");
        }
    }

ipc_done:
    // log::debug("Connection done");

    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);

    // log::debug("Disconnected pipe");
}

void InternalLoader::setupIPC() {
    std::thread([]() {
        while (!Loader::get()->isUnloading()) {
            auto pipe = CreateNamedPipeA(
                IPC_PIPE_NAME,
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                IPC_BUFFER_SIZE,
                IPC_BUFFER_SIZE,
                NMPWAIT_USE_DEFAULT_WAIT,
                nullptr
            );
            if (pipe == INVALID_HANDLE_VALUE) {
                // todo: Rn this quits IPC, but we might wanna change that later 
                // to just continue trying. however, I'm assuming that if 
                // CreateNamedPipeA fails, then it will probably fail again if 
                // you try right after, so changing the break; to continue; might 
                // just result in the console getting filled with error messages
                log::warn("Unable to create pipe, quitting IPC");
                break;
            }
            // log::debug("Waiting for pipe connections");
            if (ConnectNamedPipe(pipe, nullptr)) {
                // log::debug("Got connection, creating thread");
                std::thread(&ipcPipeThread, pipe).detach();
            } else {
                // log::debug("No connection, cleaning pipe");
                CloseHandle(pipe);
            }
        }
    }).detach();

    log::log(Severity::Debug, InternalMod::get(), "IPC set up");
}

#endif