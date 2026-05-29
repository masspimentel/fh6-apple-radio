#pragma once

#include <filesystem>
#include <string>

namespace fh6::apple_helper {

bool capture_self_process_tree_to_wav(const std::filesystem::path& output_path, int seconds);

void start_pcm_pipe_stream();
void stop_pcm_pipe_stream();

std::wstring last_capture_error();

} // namespace fh6::apple_helper