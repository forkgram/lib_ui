// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include "base/variant.h"

namespace Ui::Text {

inline constexpr auto kIsolatedEmojiLimit = 3;

struct IsolatedEmoji {
	using Item = std::variant<v::null_t, EmojiPtr, QString>;
	using Items = std::array<Item, kIsolatedEmojiLimit>;
	Items items = {};

	[[nodiscard]] bool empty() const {
		return v::is_null(items[0]);
	}
	[[nodiscard]] explicit operator bool() const {
		return !empty();
	}
	[[nodiscard]] bool operator<(const IsolatedEmoji &other) const {
		return items < other.items;
	}
	[[nodiscard]] bool operator==(const IsolatedEmoji &other) const {
		return items == other.items;
	}
	[[nodiscard]] bool operator>(const IsolatedEmoji &other) const {
		return other < *this;
	}
	[[nodiscard]] bool operator<=(const IsolatedEmoji &other) const {
		return !(other < *this);
	}
	[[nodiscard]] bool operator>=(const IsolatedEmoji &other) const {
		return !(*this < other);
	}
	[[nodiscard]] bool operator!=(const IsolatedEmoji &other) const {
		return !(*this == other);
	}
};

constexpr auto t = sizeof(IsolatedEmoji);

} // namespace Ui::Text
