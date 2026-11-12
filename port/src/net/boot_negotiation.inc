// Included after authenticated transport helpers inside netplay.cpp's namespace.
// Delay is committed and acknowledged before either guest consumes frame zero.
bool NegotiateBootDelayLocked(std::unique_lock<std::mutex>& lk) {
    const uint64_t started = MonoUsec();
    uint64_t lastPing = 0, lastAgreement = 0, lastPresent = started;
    uint8_t proposal = 0;
    while (!ps4::IsQuitRequested() && g_net.snap.phase == SessionPhase::InMatch) {
        PumpUdpRxLocked();
        const uint64_t now = MonoUsec();
        if (now - started > 15000000ULL) {
            g_net.snap.phase = SessionPhase::Error;
            g_net.snap.errorText = "Could not agree on a stable network path. Leave and retry.";
            g_net.snap.statusText = g_net.snap.errorText; return false;
        }
        if (now - lastPing >= 100000ULL) {
            SendSealedPeerPacketLocked(kPktPunch, &now, sizeof(now)); lastPing = now;
        }
        if (!proposal && now - started >= 1500000ULL && g_net.rttSamples.size() >= 6) {
            const size_t start = g_net.rttSamples.size() > 8 ? g_net.rttSamples.size() - 8 : 0;
            std::vector<double> recent(g_net.rttSamples.begin() + start, g_net.rttSamples.end());
            std::vector<double> jitter;
            for (size_t i = 1; i < recent.size(); ++i) jitter.push_back(std::fabs(recent[i] - recent[i - 1]));
            std::sort(recent.begin(), recent.end()); std::sort(jitter.begin(), jitter.end());
            const double rtt90 = recent[static_cast<size_t>((recent.size() - 1) * .9)];
            const double jitter90 = jitter.empty() ? 0 : jitter[static_cast<size_t>((jitter.size() - 1) * .9)];
            const int needed = static_cast<int>(std::ceil((rtt90 * .5 + jitter90 + 10.0) / (1000.0 / 60.0)));
            proposal = static_cast<uint8_t>(std::clamp(g_net.autoDelay ? std::max(g_net.configuredDelay, needed) : g_net.configuredDelay, 1, 12));
            LOG_INFO("[netplay] calibration role=%s rtt90=%.2fms jitter90=%.2fms proposal=%uf auto=%d",
                g_net.snap.isHost ? "host" : "guest", rtt90, jitter90, proposal, g_net.autoDelay ? 1 : 0);
        }
        if (g_net.snap.isHost && proposal && g_net.peerDelayProposal && !g_net.bootCommitted) {
            g_net.snap.inputDelayFrames = g_net.autoDelay ? std::max<int>(proposal, g_net.peerDelayProposal) : proposal;
            g_net.bootCommitted = true;
        }
        const bool start = g_net.snap.isHost ? g_net.bootCommitted && g_net.bootAcked : g_net.bootStartSeen;
        if (now - lastAgreement >= 50000ULL || start) {
            BootAgreementPayload agreement{};
            agreement.delay = g_net.bootCommitted ? static_cast<uint8_t>(g_net.snap.inputDelayFrames) : proposal;
            agreement.flags = proposal ? 1 : 0;
            if (g_net.snap.isHost && g_net.bootCommitted) agreement.flags |= 2;
            if (!g_net.snap.isHost && g_net.bootCommitted) agreement.flags |= 4;
            if (g_net.snap.isHost && start) agreement.flags |= 8;
            if (agreement.delay) SendSealedPeerPacketLocked(kPktBootAgreement, &agreement, sizeof(agreement));
            lastAgreement = now;
        }
        FlushTxQueueLocked();
        if (start) {
            g_net.bootAgreementDone = true;
            g_net.localInputs.clear(); g_net.remoteInputs.clear();
            for (int f = 0; f < g_net.snap.inputDelayFrames; ++f) {
                g_net.localInputs[f] = 0; g_net.remoteInputs[f] = 0;
            }
            g_net.rttSamples.clear(); g_net.jitterSamples.clear();
            LOG_INFO("[netplay] agreed delay=%df sampled input floor=%.1fms transport=%s",
                g_net.snap.inputDelayFrames, g_net.snap.inputDelayFrames * (1000.0 / 60.0),
                g_net.snap.transport == TransportMode::DirectP2P ? "DirectP2P" :
                UdpRelayHealthyLocked() ? "UDPRelay" : "WSSRelay");
            return true;
        }
        lk.unlock();
        if (now - lastPresent >= 33000ULL) {
            gnm::PresentCachedFrame(ps4::GameWindow()); lastPresent = now;
        }
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        Sleep(1); lk.lock();
    }
    return false;
}
