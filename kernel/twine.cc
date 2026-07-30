#include "kernel/twine.h"
#include "kernel/log.h"

YOSYS_NAMESPACE_BEGIN

std::vector<Twine> TwinePool::globals_;

IdString twine_populate(std::string name) {
	// Globals store content only: drop the prepended '\'. Publicity lives
	// in the publicity bit on the ID:: handle, not in the stored string.
	log_assert(name[0] == '\\');
	name = name.substr(1);
	TwinePool::globals_.push_back(Twine::Leaf{std::move(name)});
	return TwinePool::globals_.size() - 1;
}
void twine_prepopulate() {
	if (TwinePool::globals_.size() == STATIC_TWINE_END)
		return;
	log_assert(TwinePool::globals_.empty());
	TwinePool::globals_.reserve(STATIC_TWINE_END);
#define X(_id) twine_populate("\\" #_id);
#include "kernel/constids.inc"
#undef X
}

YOSYS_NAMESPACE_END
