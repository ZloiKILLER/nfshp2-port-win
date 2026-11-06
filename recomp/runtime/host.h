// What the program needs from the platform to run as a game of its own (host.cpp).
#pragma once
#include <string>

std::string host_exe_dir();             // the exe's folder, with the separator at the end
void host_log_start();                  // started without a console: the log goes to speed.log
void host_fatal(const std::string &msg);    // tell the player; a log nobody watches is not enough
