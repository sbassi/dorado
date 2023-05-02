#pragma once
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dorado {

class MessageSink;
struct ReadGroup;

typedef std::map<int, std::vector<uint8_t*>> channel_to_read_id_t;

class DataLoader {
public:
    DataLoader(MessageSink& read_sink,
               const std::string& device,
               size_t num_worker_threads,
               size_t max_reads = 0,
               std::optional<std::unordered_set<std::string>> read_list = std::nullopt);
    void load_reads(const std::string& path, bool recursive_file_loading = false);
    void load_read_channels(std::string data_path, bool recursive_file_loading = false);

    static std::unordered_map<std::string, ReadGroup> load_read_groups(
            std::string data_path,
            std::string model_path,
            bool recursive_file_loading = false);

    static int get_num_reads(
            std::string data_path,
            std::optional<std::unordered_set<std::string>> read_list = std::nullopt,
            bool recursive_file_loading = false);

    static uint16_t get_sample_rate(std::string data_path, bool recursive_file_loading = false);

private:
    void load_fast5_reads_from_file(const std::string& path);
    void load_pod5_reads_from_file(const std::string& path);
    void load_pod5_reads_from_file_by_read_ids(const std::string& path,
                                               const std::vector<uint8_t*>& read_ids);
    MessageSink& m_read_sink;  // Where should the loaded reads go?
    size_t m_loaded_read_count{0};
    std::string m_device;
    size_t m_num_worker_threads{1};
    size_t m_max_reads{0};
    std::optional<std::unordered_set<std::string>> m_allowed_read_ids;
    std::unordered_map<std::string, std::unordered_map<std::string, std::pair<int, int>>>
            m_file_to_bidx_ridx;

    std::unordered_map<std::string, channel_to_read_id_t> m_file_read_order;
    int m_max_channel{0};
};

}  // namespace dorado
