#include "utils.hpp"
#include "list.hpp"
/********************************* List *************************************/
/*
 * These are non-NULL pointers that will result in page faults
 * under normal circumstances, used to verify that nobody uses
 * non-initialized list entries.
 */
#define POISON_POINTER_DELTA    0xdead000000000000
#define LIST_POISON1  (list_head*)((char *) 0x00100100 + POISON_POINTER_DELTA)
#define LIST_POISON2  (list_head*)((char *) 0x00200200 + POISON_POINTER_DELTA)

void list_del(struct list_head *entry) {
	const struct list_head *prev = entry->prev, *next = entry->next;
	BUG_ON(next == LIST_POISON1, "list_del corruption, %p->next is LIST_POISON1 (%p)\n", entry, LIST_POISON1);
	BUG_ON(prev == LIST_POISON2, "list_del corruption, %p->prev is LIST_POISON2 (%p)\n", entry, LIST_POISON2);
	BUG_ON(prev->next != entry , "list_del corruption. prev->next should be %p, but was %p\n", entry, prev->next);
	BUG_ON(next->prev != entry,  "list_del corruption. next->prev should be %p, but was %p\n", entry, next->prev);
	__list_del(entry->prev, entry->next);
	entry->next = LIST_POISON1;
	entry->prev = LIST_POISON2;
}

static inline void __list_add(struct list_head *New, struct list_head *prev, struct list_head *next){
	BUG_ON(next->prev != prev, "list_add corruption. next->prev should be prev (%p), but was %p. (next=%p).\n", prev, next->prev, next);
	BUG_ON(prev->next != next, "list_add corruption. prev->next should be next (%p), but was %p. (prev=%p).\n", next, prev->next, prev);
	BUG_ON(New == prev || New == next, "list_add double add: new=%p, prev=%p, next=%p.\n", New, prev, next);
	next->prev = New;
	New->next  = next;
	New->prev  = prev;
	prev->next = New;
}

bool is_in_list(const struct list_head *entry){
	if ((entry->next == entry->prev)&&(entry->prev == entry))
		return false;									// This entry is not part of any list
	if ((entry->next == LIST_POISON1)||(entry->prev == LIST_POISON2))
		return false;									// This entry was just removed from the list
	return true;
}

void list_add(struct list_head *New, struct list_head *head){
	__list_add(New, head, head->next);
}

void __list_cut_position(struct list_head *list, struct list_head *head, struct list_head *entry){
	struct list_head *new_first = entry->next;
	list->next = head->next;
	list->next->prev = list;
	list->prev = entry;
	entry->next = list;
	head->next = new_first;
	new_first->prev = head;
}

void list_add_tail(struct list_head *New, struct list_head *head){
	__list_add(New, head->prev, head);
}

/*****************************************************************************/
// EOF.
