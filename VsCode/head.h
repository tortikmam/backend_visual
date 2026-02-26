#ifndef BACKEND_H
#define BACKEND_H

#include <string>

void start_server();

std::string get_last_message();
void send_response(const std::string& message);

#endif