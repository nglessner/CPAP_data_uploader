#ifndef UPLOAD_FSM_H
#define UPLOAD_FSM_H

// Upload FSM states — shared between main.cpp and WebServer
enum class UploadState {
    IDLE,
    LISTENING,
    ACQUIRING,
    UPLOADING,
    RELEASING,
    COOLDOWN,
    COMPLETE,
    MONITORING,
    O2RING_SYNC,
    O2RING_RETRY        // post-COOLDOWN low-duty retry on prior O2RING_SYNC miss
};

inline const char* getStateName(UploadState state) {
    switch (state) {
        case UploadState::IDLE: return "IDLE";
        case UploadState::LISTENING: return "LISTENING";
        case UploadState::ACQUIRING: return "ACQUIRING";
        case UploadState::UPLOADING: return "UPLOADING";
        case UploadState::RELEASING: return "RELEASING";
        case UploadState::COOLDOWN: return "COOLDOWN";
        case UploadState::COMPLETE: return "COMPLETE";
        case UploadState::MONITORING: return "MONITORING";
        case UploadState::O2RING_SYNC: return "O2RING_SYNC";
        case UploadState::O2RING_RETRY: return "O2RING_RETRY";
        default: return "UNKNOWN";
    }
}

#endif // UPLOAD_FSM_H
