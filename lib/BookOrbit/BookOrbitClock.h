#pragma once

namespace bookorbit {

// Makes the process clock (time()) trustworthy before a verified TLS
// handshake: wolfSSL rejects every certificate as not-yet-valid while the
// system time still reads 1970. Seeds from the RTC when the board has one,
// otherwise runs a bounded SNTP exchange (Wi-Fi must be connected). Returns
// true when time() is plausible afterwards; a false return leaves the
// caller to decide whether an unverified date check is acceptable (it is not,
// so the session reports the trust failure instead).
bool ensureSystemClock();

}  // namespace bookorbit
