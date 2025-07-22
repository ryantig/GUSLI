/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
/*****************************************************************************/
// C style double connected linked list (Queue)
// @ptr:	the list head to take the element from. @pos:    the type * to cursor. @type:	the type of the struct this is embedded in. @member:	the name of the list_struct within the struct. Note, that list is expected to be not empty.
struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)
static inline void INIT_LIST_HEAD(         struct list_head *list){ list->next = list->prev = list; }
static inline int list_empty(        const struct list_head *head){ return head->next == head; }
static inline int list_is_last(const struct list_head *list, const struct list_head *head){return list->next == head;}
static inline int list_is_singular(const struct list_head *head){ return !list_empty(head) && (head->next == head->prev);}
static inline void __list_splice(const struct list_head *list,struct list_head *prev,struct list_head *next) {
	struct list_head *first = list->next;
	struct list_head *last = list->prev;
	first->prev = prev;
	prev->next = first;
	last->next = next;
	next->prev = last;
}
static inline void list_splice(const struct list_head *list, struct list_head *head) {
	if (!list_empty(list))
		__list_splice(list, head, head->next);
}
static inline void list_splice_init(struct list_head *list, struct list_head *head) {
	if (!list_empty(list)) {
		__list_splice(list, head, head->next);
		INIT_LIST_HEAD(list);
	}
}
static inline void list_splice_tail_init(struct list_head *list, struct list_head *head) {
	if (!list_empty(list)) {
		__list_splice(list, head->prev, head);
		INIT_LIST_HEAD(list);
	}
}

static inline void __list_del(     struct list_head * prev, struct list_head * next){ next->prev = prev; prev->next = next; }
static inline void __list_del_entry(struct list_head *entry){ __list_del(entry->prev, entry->next); }
			  void   list_del(     struct list_head *entry);
static inline void   list_del_init(struct list_head *entry){ __list_del_entry(entry); INIT_LIST_HEAD(entry);}
			  bool is_in_list(     const struct list_head *entry);		// Daniel's debug method. Return true if this entry is inside a list. If it was deleted or just initialized returns 0
			  void   list_add(     struct list_head *New, struct list_head *head);
			  void list_add_tail(  struct list_head *New, struct list_head *head);
static inline void list_move(      struct list_head *New, struct list_head *head){ __list_del_entry(New); list_add(     New, head); }
static inline void list_move_tail( struct list_head *New, struct list_head *head){ __list_del_entry(New); list_add_tail(New, head); }
			  int list_calc_size(const struct list_head *head);
#define list_entry(ptr, type, member) container_of(ptr, type, member)
#define list_last_entry(         ptr, type, member) list_entry((ptr)->prev, type, member)
#define list_first_entry(ptr, type, member) list_entry((ptr)->next, type, member)
#define list_first_entry_or_null(ptr, type, member) (!list_empty(ptr) ? list_first_entry(ptr, type, member) : (type*)NULL)
#define list_next_entry(               pos, member) list_entry((pos)->member.next, typeof(*(pos)), member)
#define list_prev_entry(               pos, member) list_entry((pos)->member.prev, typeof(*(pos)), member)
#define list_for_each_entry(pos, head, member)	       for (pos = list_entry((head)->next, typeof(*pos), member);  														  &pos->member != (head); pos        = list_entry(pos->member.next, typeof(*pos), member))
#define list_for_each_entry_safe(pos, n, head, member) for (pos = list_entry((head)->next, typeof(*pos), member), n = list_entry(pos->member.next, typeof(*pos), member); &pos->member != (head); pos = n, n = list_entry(pos->member.next, typeof(*pos), member))
#define list_for_each(pos, head) 					   for (pos = (head)->next; pos != (head); pos = pos->next)
#define list_for_each_safe(pos, n, head) 			   for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)


/*****************************************************************************/
// EOF.
