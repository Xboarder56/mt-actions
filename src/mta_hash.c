#include "mt_actions_internal.h"
#include <stdlib.h>

void mta_hash_table_init(mta_hash_table_t* table) {
    if (table == NULL) return;
    table->buckets = NULL;
    table->reserved = 0;
    table->size = 0;
}

void mta_hash_table_free(mta_hash_table_t* table) {
    if (table == NULL) return;
    free(table->buckets);
    table->buckets = NULL;
    table->reserved = 0;
    table->size = 0;
}
