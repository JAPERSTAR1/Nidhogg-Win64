// Included inside overlay.cpp's private namespace, after clipboard helpers.
// Only immutable action arguments cross threads. UI state changes on the UI thread.
struct OnlineResult {
    bool ok = false, noRooms = false, hasRooms = false, closeOverlay = false, home = false;
    std::string error;
    std::vector<net::PublicRoomEntry> rooms;
};
std::future<OnlineResult> g_onlineTask;

void QueueOnline(std::function<OnlineResult()> work) {
    if (g_onlineTask.valid()) return;
    g_ui.onlineBanner = "Connecting..."; g_ui.onlineBannerIsError = false;
    g_onlineTask = std::async(std::launch::async, std::move(work));
}
void QueueSimple(std::function<bool(std::string&)> work, bool home = false, bool close = false) {
    QueueOnline([work = std::move(work), home, close] {
        OnlineResult r; r.ok = work(r.error); r.home = home; r.closeOverlay = close; return r;
    });
}
void RefreshRoomsAsync() {
    const auto region = g_ui.browserRegion; const bool available = g_ui.browserAvailableOnly;
    QueueOnline([region, available] {
        OnlineResult r; r.hasRooms = true;
        r.ok = net::RefreshPublicRooms(region, available, r.rooms, r.error); return r;
    });
}
void PollOnlineAction() {
    if (!g_onlineTask.valid() || g_onlineTask.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    auto result = g_onlineTask.get();
    g_ui.onlineBannerIsError = !result.ok && !result.noRooms;
    g_ui.onlineBanner = result.ok ? "Connected." : result.error;
    g_ui.offerCreateAfterQuickMatch = result.noRooms;
    if (result.ok) {
        if (result.hasRooms) {
            g_ui.publicRooms = std::move(result.rooms);
            g_ui.onlineBanner = "Loaded " + std::to_string(g_ui.publicRooms.size()) + " public room(s).";
        }
        if (result.home) g_ui.onlinePage = OnlineSubPage::Home;
        if (result.closeOverlay) g_ui.open = false;
    } else if(!result.hasRooms) { g_ui.open = true; g_ui.tab = net::GetSnapshot().roomId.empty()?6:4; }
}
void HandleOnlineAction(int row, int dir, bool activated, bool& save) {
    if (g_onlineTask.valid()) return;
    auto& cfg = core::CurrentSettings();
    const auto snap = net::GetSnapshot();
    const bool active = snap.phase == net::SessionPhase::LobbyWaiting ||
        snap.phase == net::SessionPhase::ConnectingTransport || snap.phase == net::SessionPhase::SyncingMenu ||
        snap.phase == net::SessionPhase::InMatch ||
        ((snap.phase == net::SessionPhase::Error || snap.phase == net::SessionPhase::Disconnected) && !snap.roomId.empty());
    if (active) {
        if(row==1 && snap.isHost) {
            std::string error;
            if(!net::SelectRoomStage((snap.stageIndex+(dir?dir:1)+4)%4,error)) g_ui.onlineBanner=error;
            return;
        }
        if (!activated) return;
        switch (row) {
            case 0: if (net::CopyRoomCodeToClipboard()) g_ui.onlineBanner = "Room Code copied."; break;
            case 2: if (snap.isHost) QueueSimple([pub = !snap.isPublic](std::string& e) { return net::SetRoomVisibility(pub, e); }); break;
            case 3: if (snap.isHost && snap.remoteConnected) QueueSimple([](std::string& e) { return net::StartMatch(e); }); break;
            case 4: if (snap.isHost) QueueSimple([](std::string& e) { return net::KickGuest(e); }); break;
            case 5: QueueSimple([](std::string&) { net::LeaveRoom(); return true; }, true);g_ui.tab=6;g_ui.selectedRow=0;break;
        }
    } else if (g_ui.onlinePage == OnlineSubPage::Home) {
        if (row == 5) { g_ui.createStageIndex = (g_ui.createStageIndex + (dir ? dir : 1) + 4) % 4; return; }
        if (!activated) return;
        const int stage = g_ui.createStageIndex;
        switch (row) {
            case 0:
                if (g_ui.offerCreateAfterQuickMatch) {
                    g_ui.offerCreateAfterQuickMatch = false;
                    QueueSimple([stage](std::string& e) { return net::CreateRoom("", true, stage, e); });
                } else QueueOnline([region = cfg.region] {
                    OnlineResult r; r.ok = net::QuickMatch(region, r.noRooms, r.error); return r;
                });
                break;
            case 1: g_ui.onlinePage = OnlineSubPage::Browser; g_ui.selectedRow = 0; RefreshRoomsAsync(); break;
            case 2: case 3: QueueSimple([stage, pub = row == 2](std::string& e) { return net::CreateRoom("", pub, stage, e); }); break;
            case 4: g_ui.onlinePage = OnlineSubPage::JoinCode; g_ui.editingText = EditTarget::RoomCode; g_ui.selectedRow = 0; break;
            case 6: g_ui.onlinePage = OnlineSubPage::OnlineSettings; g_ui.selectedRow = 0; break;
        }
    } else if (g_ui.onlinePage == OnlineSubPage::Browser) {
        if (row == 0 && activated) RefreshRoomsAsync();
        else if (row == 1) {
            const char* regions[] = {"all", "us-east", "us-west", "eu-west", "ap-east"}; int index = 0;
            for (int i = 0; i < 5; ++i) if (g_ui.browserRegion == regions[i]) index = i;
            g_ui.browserRegion = regions[(index + (dir ? dir : 1) + 5) % 5]; RefreshRoomsAsync();
        } else if (row == 2) { g_ui.browserAvailableOnly = !g_ui.browserAvailableOnly; RefreshRoomsAsync(); }
        else if (row == 3 && activated) { g_ui.onlinePage = OnlineSubPage::Home; g_ui.selectedRow = 1; }
        else if (row >= 4 && activated && static_cast<size_t>(row - 4) < g_ui.publicRooms.size())
            QueueSimple([id = g_ui.publicRooms[row - 4].roomId](std::string& e) { return net::JoinRoomById(id, e); }, true);
    } else if (g_ui.onlinePage == OnlineSubPage::JoinCode) {
        if (!activated) return;
        if (row == 0) g_ui.editingText = EditTarget::RoomCode;
        else if (row == 1) {
            g_ui.joinCodeInput.clear();
            for (unsigned char c : ReadClipboardText()) if (std::isalnum(c) && g_ui.joinCodeInput.size() < 6)
                g_ui.joinCodeInput.push_back(static_cast<char>(std::toupper(c)));
        } else if (row == 2) {
            g_ui.editingText = EditTarget::None;
            QueueSimple([code = g_ui.joinCodeInput](std::string& e) { return net::JoinRoomByCode(code, e); }, true);
        } else if (row == 3) { g_ui.editingText = EditTarget::None; g_ui.onlinePage = OnlineSubPage::Home; g_ui.selectedRow = 4; }
    } else if (g_ui.onlinePage == OnlineSubPage::OnlineSettings) {
        switch (row) {
            case 0: if (activated) g_ui.editingText = g_ui.editingText == EditTarget::PlayerName ? EditTarget::None : EditTarget::PlayerName; break;
            case 1: {
                const char* regions[] = {"us-east", "us-west", "eu-west", "ap-east"}; int index = 0;
                for (int i = 0; i < 4; ++i) if (cfg.region == regions[i]) index = i;
                cfg.region = regions[(index + (dir ? dir : 1) + 4) % 4]; save = true; break;
            }
            case 2:
                if (activated) cfg.autoInputDelay = !cfg.autoInputDelay;
                else cfg.inputDelayFrames = std::clamp(cfg.inputDelayFrames + dir, 1, 12);
                save = true; break;
            case 3: if (activated) g_ui.editingText = g_ui.editingText == EditTarget::ServerUrl ? EditTarget::None : EditTarget::ServerUrl; break;
            case 4: if (activated) { g_ui.editingText = EditTarget::None; g_ui.onlinePage = OnlineSubPage::Home; g_ui.tab=g_ui.fromMultiplayer?6:0;g_ui.selectedRow=0; } break;
        }
    }
}
