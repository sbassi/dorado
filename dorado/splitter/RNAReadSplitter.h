#pragma once
#include "ReadSplitter.h"
#include "splitter/splitter_utils.h"

#include <functional>
#include <string>
#include <vector>

namespace dorado::splitter {

class RNAReadSplitter : public ReadSplitter {
public:
    RNAReadSplitter(RNASplitSettings settings);

    std::vector<SimplexReadPtr> split(SimplexReadPtr init_read) const override;

private:
    //TODO consider precomputing and reusing ranges with high signal
    struct ExtRead {
        SimplexReadPtr read;
        splitter::SampleRanges possible_pore_regions;
    };

    using SplitFinderF = std::function<splitter::SampleRanges(const ExtRead&)>;

    ExtRead create_ext_read(SimplexReadPtr r) const;
    SampleRanges possible_pore_regions(const ExtRead& read) const;

    std::vector<SimplexReadPtr> subreads(SimplexReadPtr read,
                                         const splitter::SampleRanges& spacers) const;

    std::vector<std::pair<std::string, SplitFinderF>> build_split_finders() const;

    const RNASplitSettings m_settings;
    std::vector<std::pair<std::string, SplitFinderF>> m_split_finders;
};

}  // namespace dorado::splitter
