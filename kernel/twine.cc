#include "kernel/twine.h"
#include "kernel/log.h"

YOSYS_NAMESPACE_BEGIN

std::vector<TwineNode> StaticTwines::nodes_;

void StaticTwines::init() {
	if (ready())
		return;
	log_assert(nodes_.empty());
	nodes_.reserve(count);
#define X(_id) nodes_.push_back(Twine::Leaf{#_id});
#include "kernel/constids.inc"
#undef X
}

void twine_prepopulate() { StaticTwines::init(); }

int64_t twine_gc_ns;
int twine_gc_count;

YOSYS_NAMESPACE_END
