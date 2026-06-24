#pragma once

#include <cstddef>

#include "common/ITypes.h"

struct DialogueStringView {
	char* m_data;
	UInt16 m_dataLen;
	UInt16 m_bufLen;

	const char* c_str() const { return m_data ? m_data : ""; }
};

struct DialogueResponseView {
	DialogueStringView responseText;
	UInt32 emotionType;
	SInt32 emotionValue;
	DialogueStringView voiceFileName;
	void* speakerIdle;
	void* listenerIdle;
	void* sound;
	UInt32 unk024;
	UInt32 unk028;
};

template <typename T>
struct BSSimpleListNodeView {
	T item;
	BSSimpleListNodeView<T>* next;
};

struct DialogueItemView {
	BSSimpleListNodeView<DialogueResponseView*> responses;
	BSSimpleListNodeView<DialogueResponseView*>* currentResponse;
	void* currentTopicInfo;
	void* currentTopic;
	void* currentQuest;
	void* currentSpeaker;
};

struct ConversationView {
	BSSimpleListNodeView<DialogueItemView*> dialogueItems;
	BSSimpleListNodeView<DialogueItemView*>* currentItem;
};

struct CharacterView {
	UInt8 pad00[0x08];
	UInt32 flags;
	UInt8 pad0C[0x1BC];
};

static_assert(sizeof(DialogueStringView) == 0x08);
static_assert(offsetof(DialogueStringView, m_data) == 0x00);
static_assert(offsetof(DialogueStringView, m_dataLen) == 0x04);
static_assert(offsetof(DialogueStringView, m_bufLen) == 0x06);

static_assert(sizeof(DialogueResponseView) == 0x2C);
static_assert(offsetof(DialogueResponseView, responseText) == 0x00);
static_assert(offsetof(DialogueResponseView, emotionType) == 0x08);
static_assert(offsetof(DialogueResponseView, emotionValue) == 0x0C);
static_assert(offsetof(DialogueResponseView, voiceFileName) == 0x10);
static_assert(offsetof(DialogueResponseView, speakerIdle) == 0x18);
static_assert(offsetof(DialogueResponseView, listenerIdle) == 0x1C);

static_assert(sizeof(BSSimpleListNodeView<DialogueItemView*>) == 0x08);

static_assert(sizeof(DialogueItemView) == 0x1C);
static_assert(offsetof(DialogueItemView, responses) == 0x00);
static_assert(offsetof(DialogueItemView, currentResponse) == 0x08);
static_assert(offsetof(DialogueItemView, currentTopicInfo) == 0x0C);
static_assert(offsetof(DialogueItemView, currentTopic) == 0x10);
static_assert(offsetof(DialogueItemView, currentQuest) == 0x14);
static_assert(offsetof(DialogueItemView, currentSpeaker) == 0x18);

static_assert(sizeof(ConversationView) == 0x0C);
static_assert(offsetof(ConversationView, dialogueItems) == 0x00);
static_assert(offsetof(ConversationView, currentItem) == 0x08);

static_assert(sizeof(CharacterView) == 0x1C8);
static_assert(offsetof(CharacterView, flags) == 0x08);
