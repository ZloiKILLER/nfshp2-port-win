// What the program needs from the platform to run as a game of its own (host.cpp).
#pragma once
#include <string>

std::string host_exe_dir();             // the exe's folder, with the separator at the end
void host_log_start(bool wanted);       // started without a console: the log goes to speed.log (4 MB at most), or nowhere if not wanted
void host_log_close();                  // speed.log is finished and let go: before this program starts itself again, which opens it anew
void host_log_flush();                  // what was written is in speed.log when this returns: before the program ends
void host_fatal(const std::string &msg);    // tell the player; a log nobody watches is not enough
