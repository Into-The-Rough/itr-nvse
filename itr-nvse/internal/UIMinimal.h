#pragma once

#include <cstddef>

#include "common/ITypes.h"

namespace UIMinimal
{
	enum MenuSpecialKeyboardInputCode
	{
		kEnter = -2,
		kUpArrow = 1,
		kDownArrow,
		kRightArrow,
		kLeftArrow,
		kSpace = 9,
		kTab,
		kShiftEnter,
		kAltEnter,
		kShiftLeft,
		kShiftRight,
		kPageUp,
		kPageDown
	};

	constexpr UInt32 kTileValue_id = 0xFAA;

	class Tile
	{
	public:
		struct Value
		{
			UInt32 id;
			Tile* parent;
			float num;
			char* str;
			void* action;
		};

		template <typename T>
		struct BSSimpleArray
		{
			void* vtable;
			T* data;
			UInt32 size;
			UInt32 capacity;
		};

		void* vtable;
		UInt8 pad04[0x0C];
		BSSimpleArray<Value*> values;
	};

	template <typename Item>
	struct ListBox
	{
		struct ListItem
		{
			Tile* tile;
			Item item;
			bool isFiltered;
			UInt8 pad09[3];
		};

		struct Node
		{
			ListItem* data;
			Node* next;
		};

		struct List
		{
			Node m_listHead;

			Node* Head() { return &m_listHead; }
			const Node* Head() const { return &m_listHead; }
		};

		void* vtable;
		List list;
		Tile* parentTile;
		Tile* selected;
	};

	class MessageMenu
	{
	public:
		void* vtable;
		UInt8 pad04[0x3C];
		ListBox<int> buttonList;
	};

	static_assert(sizeof(Tile::Value) == 0x14);
	static_assert(offsetof(Tile, values) == 0x10);
	static_assert(offsetof(ListBox<int>, list) == 0x04);
	static_assert(offsetof(ListBox<int>, selected) == 0x10);
	static_assert(offsetof(MessageMenu, buttonList) == 0x40);
}
