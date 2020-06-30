/* dynamic SQL support routines
 *
 * src/interfaces/ecpg/ecpglib/descriptor.c
 */

#define POSTGRES_ECPG_INTERNAL
#include "postgres_fe.h"
#include "pg_type.h"

#include "ecpg-pthread-win32.h"
#include "ecpgtype.h"
#include "ecpglib.h"
#include "ecpgerrno.h"
#include "extern.h"
#include "sqlca.h"
#include "sqlda.h"
#include "sql3types.h"

static void descriptor_free(struct descriptor* desc);

/* We manage descriptors separately for each thread. */
#ifdef ENABLE_THREAD_SAFETY
static pthread_key_t descriptor_key;
static pthread_once_t descriptor_once = PTHREAD_ONCE_INIT;

static void descriptor_deallocate_all(struct descriptor* list);

static void descriptor_destructor(void* arg)
{
    descriptor_deallocate_all((descriptor*)arg);
}

static void descriptor_key_init(void)
{
    pthread_key_create(&descriptor_key, descriptor_destructor);
}

static struct descriptor* get_descriptors(void)
{
    pthread_once(&descriptor_once, descriptor_key_init);
    return (struct descriptor*)pthread_getspecific(descriptor_key);
}

static void set_descriptors(struct descriptor* value)
{
    pthread_setspecific(descriptor_key, value);
}
#else
static struct descriptor* all_descriptors = NULL;

#define get_descriptors() (all_descriptors)
#define set_descriptors(value)     \
    do {                           \
        all_descriptors = (value); \
    } while (0)
#endif

/* old internal convenience function that might go away later */
static PGresult* ecpg_result_by_descriptor(int line, const char* name)
{
    struct descriptor* desc = ecpg_find_desc(line, name);

    if (desc == NULL)
        return NULL;
    return desc->result;
}

static unsigned int ecpg_dynamic_type_DDT(Oid type)
{
    switch (type) {
        case DATEOID:
            return SQL3_DDT_DATE;
        case TIMEOID:
            return SQL3_DDT_TIME;
        case TIMESTAMPOID:
            return SQL3_DDT_TIMESTAMP;
        case TIMESTAMPTZOID:
            return SQL3_DDT_TIMESTAMP_WITH_TIME_ZONE;
        case TIMETZOID:
            return SQL3_DDT_TIME_WITH_TIME_ZONE;
        default:
            return SQL3_DDT_ILLEGAL;
    }
}

bool ECPGget_desc_header(int lineno, const char* desc_name, int* count)
{
    PGresult* ECPGresult = NULL;
    struct sqlca_t* sqlca = ECPGget_sqlca();

    ecpg_init_sqlca(sqlca);
    ECPGresult = ecpg_result_by_descriptor(lineno, desc_name);
    if (ECPGresult == NULL)
        return false;

    *count = PQnfields(ECPGresult);
    sqlca->sqlerrd[2] = 1;
    ecpg_log("ECPGget_desc_header: found %d attributes\n", *count);
    return true;
}

static bool get_int_item(int lineno, void* var, enum ECPGttype vartype, int value)
{
    switch (vartype) {
        case ECPGt_short:
            *(short*)var = (short)value;
            break;
        case ECPGt_int:
            *(int*)var = (int)value;
            break;
        case ECPGt_long:
            *(long*)var = (long)value;
            break;
        case ECPGt_unsigned_short:
            *(unsigned short*)var = (unsigned short)value;
            break;
        case ECPGt_unsigned_int:
            *(unsigned int*)var = (unsigned int)value;
            break;
        case ECPGt_unsigned_long:
            *(unsigned long*)var = (unsigned long)value;
            break;
#ifdef HAVE_LONG_LONG_INT
        case ECPGt_long_long:
            *(long long int*)var = (long long int)value;
            break;
        case ECPGt_unsigned_long_long:
            *(unsigned long long int*)var = (unsigned long long int)value;
            break;
#endif /* HAVE_LONG_LONG_INT */
        case ECPGt_float:
            *(float*)var = (float)value;
            break;
        case ECPGt_double:
            *(double*)var = (double)value;
            break;
        default:
            ecpg_raise(lineno, ECPG_VAR_NOT_NUMERIC, ECPG_SQLSTATE_RESTRICTED_DATA_TYPE_ATTRIBUTE_VIOLATION, NULL);
            return (false);
    }

    return (true);
}

static bool set_int_item(int lineno, int* target, const void* var, enum ECPGttype vartype)
{
    switch (vartype) {
        case ECPGt_short:
            *target = *(const short*)var;
            break;
        case ECPGt_int:
            *target = *(const int*)var;
            break;
        case ECPGt_long:
            *target = *(const long*)var;
            break;
        case ECPGt_unsigned_short:
            *target = *(const unsigned short*)var;
            break;
        case ECPGt_unsigned_int:
            *target = *(const unsigned int*)var;
            break;
        case ECPGt_unsigned_long:
            *target = *(const unsigned long*)var;
            break;
#ifdef HAVE_LONG_LONG_INT
        case ECPGt_long_long:
            *target = *(const long long int*)var;
            break;
        case ECPGt_unsigned_long_long:
            *target = *(const unsigned long long int*)var;
            break;
#endif /* HAVE_LONG_LONG_INT */
        case ECPGt_float:
            *target = *(const float*)var;
            break;
        case ECPGt_double:
            *target = *(const double*)var;
            break;
        default:
            ecpg_raise(lineno, ECPG_VAR_NOT_NUMERIC, ECPG_SQLSTATE_RESTRICTED_DATA_TYPE_ATTRIBUTE_VIOLATION, NULL);
            return (false);
    }

    return true;
}

static bool get_char_item(int lineno, void* var, enum ECPGttype vartype, char* value, int varcharsize)
{
    switch (vartype) {
        case ECPGt_char:
        case ECPGt_unsigned_char:
        case ECPGt_string:
            strncpy((char*)var, value, varcharsize);
            break;
        case ECPGt_varchar: {
            struct ECPGgeneric_varchar* variable = (struct ECPGgeneric_varchar*)var;

            if (varcharsize == 0) {
                errno_t rc = memcpy_s(variable->arr, strlen(value), value, strlen(value));
                securec_check_c(rc, "\0", "\0");
            } else
                strncpy(variable->arr, value, varcharsize);

            variable->len = strlen(value);
            if (varcharsize > 0 && variable->len > varcharsize)
                variable->len = varcharsize;
        } break;
        default:
            ecpg_raise(lineno, ECPG_VAR_NOT_CHAR, ECPG_SQLSTATE_RESTRICTED_DATA_TYPE_ATTRIBUTE_VIOLATION, NULL);
            return (false);
    }

    return (true);
}

#define RETURN_IF_NO_DATA                                                \
    if (ntuples < 1) {                                                   \
        va_end(args);                                                    \
        ecpg_raise(lineno, ECPG_NOT_FOUND, ECPG_SQLSTATE_NO_DATA, NULL); \
        return (false);                                                  \
    }

bool ECPGget_desc(int lineno, const char* desc_name, int index, ...)
{
    va_list args;
    PGresult* ECPGresult = NULL;
    enum ECPGdtype type;
    int ntuples, act_tuple;
    struct variable data_var;
    struct sqlca_t* sqlca = ECPGget_sqlca();

    va_start(args, index);
    ecpg_init_sqlca(sqlca);
    ECPGresult = ecpg_result_by_descriptor(lineno, desc_name);
    if (ECPGresult == NULL) {
        va_end(args);
        return (false);
    }

    ntuples = PQntuples(ECPGresult);

    if (index < 1 || index > PQnfields(ECPGresult)) {
        ecpg_raise(lineno, ECPG_INVALID_DESCRIPTOR_INDEX, ECPG_SQLSTATE_INVALID_DESCRIPTOR_INDEX, NULL);
        va_end(args);
        return (false);
    }

    ecpg_log("ECPGget_desc: reading items for tuple %d\n", index);
    --index;

    type = (ECPGdtype)va_arg(args, int);

    memset(&data_var, 0, sizeof data_var);
    data_var.type = ECPGt_EORT;
    data_var.ind_type = ECPGt_NO_INDICATOR;

    while (type != ECPGd_EODT) {
        char type_str[20];
        long varcharsize;
        long offset;
        long arrsize;
        enum ECPGttype vartype;
        void* var = NULL;

        vartype = (ECPGttype)va_arg(args, int);
        var = va_arg(args, void*);
        varcharsize = va_arg(args, long);
        arrsize = va_arg(args, long);
        offset = va_arg(args, long);

        switch (type) {
            case (ECPGd_indicator):
                RETURN_IF_NO_DATA;
                data_var.ind_type = vartype;
                data_var.ind_pointer = var;
                data_var.ind_varcharsize = varcharsize;
                data_var.ind_arrsize = arrsize;
                data_var.ind_offset = offset;
                if (data_var.ind_arrsize == 0 || data_var.ind_varcharsize == 0)
                    data_var.ind_value = *((void**)(data_var.ind_pointer));
                else
                    data_var.ind_value = data_var.ind_pointer;
                break;

            case ECPGd_data:
                RETURN_IF_NO_DATA;
                data_var.type = vartype;
                data_var.pointer = var;
                data_var.varcharsize = varcharsize;
                data_var.arrsize = arrsize;
                data_var.offset = offset;
                if (data_var.arrsize == 0 || data_var.varcharsize == 0)
                    data_var.value = *((void**)(data_var.pointer));
                else
                    data_var.value = data_var.pointer;
                break;

            case ECPGd_name:
                if (!get_char_item(lineno, var, vartype, PQfname(ECPGresult, index), varcharsize)) {
                    va_end(args);
                    return (false);
                }

                ecpg_log("ECPGget_desc: NAME = %s\n", PQfname(ECPGresult, index));
                break;

            case ECPGd_nullable:
                if (!get_int_item(lineno, var, vartype, 1)) {
                    va_end(args);
                    return (false);
                }

                break;

            case ECPGd_key_member:
                if (!get_int_item(lineno, var, vartype, 0)) {
                    va_end(args);
                    return (false);
                }

                break;

            case ECPGd_scale:
                if (!get_int_item(lineno, var, vartype, (PQfmod(ECPGresult, index) - VARHDRSZ) & 0xffff)) {
                    va_end(args);
                    return (false);
                }

                ecpg_log("ECPGget_desc: SCALE = %d\n", (PQfmod(ECPGresult, index) - VARHDRSZ) & 0xffff);
                break;

            case ECPGd_precision:
                if (!get_int_item(lineno, var, vartype, PQfmod(ECPGresult, index) >> 16)) {
                    va_end(args);
                    return (false);
                }

                ecpg_log("ECPGget_desc: PRECISION = %d\n", PQfmod(ECPGresult, index) >> 16);
                break;

            case ECPGd_octet:
                if (!get_int_item(lineno, var, vartype, PQfsize(ECPGresult, index))) {
                    va_end(args);
                    return (false);
                }

                ecpg_log("ECPGget_desc: OCTET_LENGTH = %d\n", PQfsize(ECPGresult, index));
                break;

            case ECPGd_length:
                if (!get_int_item(lineno, var, vartype, PQfmod(ECPGresult, index) - VARHDRSZ)) {
                    va_end(args);
                    return (false);
                }

                ecpg_log("ECPGget_desc: LENGTH = %d\n", PQfmod(ECPGresult, index) - VARHDRSZ);
                break;

            case ECPGd_type:
                if (!get_int_item(lineno, var, vartype, ecpg_dynamic_type(PQftype(ECPGresult, index)))) {
                    va_end(args);
                    return (false);
                }

                ecpg_log("ECPGget_desc: TYPE = %d\n", ecpg_dynamic_type(PQftype(ECPGresult, index)));
                break;

            case ECPGd_di_code:
                if (!get_int_item(lineno, var, vartype, ecpg_dynamic_type_DDT(PQftype(ECPGresult, index)))) {
                    va_end(args);
                    return (false);
                }

                ecpg_log("ECPGget_desc: TYPE = %d\n", ecpg_dynamic_type_DDT(PQftype(ECPGresult, index)));
                break;

            case ECPGd_cardinality:
                if (!get_int_item(lineno, var, vartype, PQntuples(ECPGresult))) {
                    va_end(args);
                    return (false);
                }

                ecpg_log("ECPGget_desc: CARDINALITY = %d\n", PQntuples(ECPGresult));
                break;

            case ECPGd_ret_length:
            case ECPGd_ret_octet:

                RETURN_IF_NO_DATA;

                /*
                 * this is like ECPGstore_result
                 */
                if (arrsize > 0 && ntuples > arrsize) {
                    ecpg_log("ECPGget_desc on line %d: incorrect number of matches; %d don't fit into array of %ld\n",
                        lineno,
                        ntuples,
                        arrsize);
                    ecpg_raise(lineno, ECPG_TOO_MANY_MATCHES, ECPG_SQLSTATE_CARDINALITY_VIOLATION, NULL);
                    va_end(args);
                    return false;
                }
                /* allocate storage if needed */
                if (arrsize == 0 && *(void**)var == NULL) {
                    void* mem = (void*)ecpg_alloc(offset * ntuples, lineno);

                    if (mem == NULL) {
                        va_end(args);
                        return false;
                    }
                    *(void**)var = mem;
                    ecpg_add_mem(mem, lineno);
                    var = mem;
                }

                for (act_tuple = 0; act_tuple < ntuples; act_tuple++) {
                    if (!get_int_item(lineno, var, vartype, PQgetlength(ECPGresult, act_tuple, index))) {
                        va_end(args);
                        return (false);
                    }
                    var = (char*)var + offset;
                    ecpg_log("ECPGget_desc: RETURNED[%d] = %d\n", act_tuple, PQgetlength(ECPGresult, act_tuple, index));
                }
                break;

            default:
                snprintf(type_str, sizeof(type_str), "%d", type);
                ecpg_raise(lineno, ECPG_UNKNOWN_DESCRIPTOR_ITEM, ECPG_SQLSTATE_ECPG_INTERNAL_ERROR, type_str);
                va_end(args);
                return (false);
        }

        type = (ECPGdtype)va_arg(args, int);
    }

    if (data_var.type != ECPGt_EORT) {
        struct statement stmt;
        char* oldlocale = NULL;

        /* Make sure we do NOT honor the locale for numeric input */
        /* since the database gives the standard decimal point */
        oldlocale = ecpg_strdup(setlocale(LC_NUMERIC, NULL), lineno);
        setlocale(LC_NUMERIC, "C");

        memset(&stmt, 0, sizeof stmt);
        stmt.lineno = lineno;

        /* desperate try to guess something sensible */
        stmt.connection = ecpg_get_connection(NULL);
        ecpg_store_result(ECPGresult, index, &stmt, &data_var);

        setlocale(LC_NUMERIC, oldlocale);
        ecpg_free(oldlocale);
    } else if (data_var.ind_type != ECPGt_NO_INDICATOR && data_var.ind_pointer != NULL)

    /*
     * ind_type != NO_INDICATOR should always have ind_pointer != NULL but
     * since this might be changed manually in the .c file let's play it
     * safe
     */
    {
        /*
         * this is like ECPGstore_result but since we don't have a data
         * variable at hand, we can't call it
         */
        if (data_var.ind_arrsize > 0 && ntuples > data_var.ind_arrsize) {
            ecpg_log(
                "ECPGget_desc on line %d: incorrect number of matches (indicator); %d don't fit into array of %ld\n",
                lineno,
                ntuples,
                data_var.ind_arrsize);
            ecpg_raise(lineno, ECPG_TOO_MANY_MATCHES, ECPG_SQLSTATE_CARDINALITY_VIOLATION, NULL);
            va_end(args);
            return false;
        }

        /* allocate storage if needed */
        if (data_var.ind_arrsize == 0 && data_var.ind_value == NULL) {
            void* mem = (void*)ecpg_alloc(data_var.ind_offset * ntuples, lineno);

            if (mem == NULL) {
                va_end(args);
                return false;
            }
            *(void**)data_var.ind_pointer = mem;
            ecpg_add_mem(mem, lineno);
            data_var.ind_value = mem;
        }

        for (act_tuple = 0; act_tuple < ntuples; act_tuple++) {
            if (!get_int_item(
                    lineno, data_var.ind_value, data_var.ind_type, -PQgetisnull(ECPGresult, act_tuple, index))) {
                va_end(args);
                return (false);
            }
            data_var.ind_value = (char*)data_var.ind_value + data_var.ind_offset;
            ecpg_log("ECPGget_desc: INDICATOR[%d] = %d\n", act_tuple, -PQgetisnull(ECPGresult, act_tuple, index));
        }
    }
    sqlca->sqlerrd[2] = ntuples;
    va_end(args);
    return (true);
}

#undef RETURN_IF_NO_DATA

bool ECPGset_desc_header(int lineno, const char* desc_name, int count)
{
    struct descriptor* desc = ecpg_find_desc(lineno, desc_name);

    if (desc == NULL)
        return false;
    desc->count = count;
    return true;
}

bool ECPGset_desc(int lineno, const char* desc_name, int index, ...)
{
    va_list args;
    struct descriptor* desc;
    struct descriptor_item* desc_item;
    struct variable* var;

    desc = ecpg_find_desc(lineno, desc_name);
    if (desc == NULL)
        return false;

    for (desc_item = desc->items; desc_item != NULL; desc_item = desc_item->next) {
        if (desc_item->num == index)
            break;
    }

    if (desc_item == NULL) {
        desc_item = (struct descriptor_item*)ecpg_alloc(sizeof(*desc_item), lineno);
        if (desc_item == NULL)
            return false;
        desc_item->num = index;
        if (desc->count < index)
            desc->count = index;
        desc_item->next = desc->items;
        desc->items = desc_item;
    }

    if ((var = (struct variable*)ecpg_alloc(sizeof(struct variable), lineno)) == NULL)
        return false;

    va_start(args, index);

    for (;;) {
        enum ECPGdtype itemtype;
        char* tobeinserted = NULL;

        itemtype = (ECPGdtype)va_arg(args, int);

        if (itemtype == ECPGd_EODT)
            break;

        var->type = (ECPGttype)va_arg(args, int);
        var->pointer = va_arg(args, char*);

        var->varcharsize = va_arg(args, long);
        var->arrsize = va_arg(args, long);
        var->offset = va_arg(args, long);

        if (var->arrsize == 0 || var->varcharsize == 0)
            var->value = *((char**)(var->pointer));
        else
            var->value = var->pointer;

        /*
         * negative values are used to indicate an array without given bounds
         */
        /* reset to zero for us */
        if (var->arrsize < 0)
            var->arrsize = 0;
        if (var->varcharsize < 0)
            var->varcharsize = 0;

        var->next = NULL;

        switch (itemtype) {
            case ECPGd_data: {
                if (!ecpg_store_input(lineno, true, var, &tobeinserted, false)) {
                    ecpg_free(var);
                    va_end(args);
                    return false;
                }

                ecpg_free(desc_item->data); /* free() takes care of a
                                             * potential NULL value */
                desc_item->data = (char*)tobeinserted;
                tobeinserted = NULL;
                break;
            }

            case ECPGd_indicator:
                set_int_item(lineno, &desc_item->indicator, var->pointer, var->type);
                break;

            case ECPGd_length:
                set_int_item(lineno, &desc_item->length, var->pointer, var->type);
                break;

            case ECPGd_precision:
                set_int_item(lineno, &desc_item->precision, var->pointer, var->type);
                break;

            case ECPGd_scale:
                set_int_item(lineno, &desc_item->scale, var->pointer, var->type);
                break;

            case ECPGd_type:
                set_int_item(lineno, &desc_item->type, var->pointer, var->type);
                break;

            default: {
                char type_str[20];

                snprintf(type_str, sizeof(type_str), "%d", itemtype);
                ecpg_raise(lineno, ECPG_UNKNOWN_DESCRIPTOR_ITEM, ECPG_SQLSTATE_ECPG_INTERNAL_ERROR, type_str);
                ecpg_free(var);
                va_end(args);
                return false;
            }
        }
    }
    ecpg_free(var);
    va_end(args);

    return true;
}

/* Free the descriptor and items in it. */
static void descriptor_free(struct descriptor* desc)
{
    struct descriptor_item* desc_item;

    for (desc_item = desc->items; desc_item != NULL;) {
        struct descriptor_item* di;

        ecpg_free(desc_item->data);
        di = desc_item;
        desc_item = desc_item->next;
        ecpg_free(di);
    }

    ecpg_free(desc->name);
    PQclear(desc->result);
    ecpg_free(desc);
}

bool ECPGdeallocate_desc(int line, const char* name)
{
    struct descriptor* desc;
    struct descriptor* prev;
    struct sqlca_t* sqlca = ECPGget_sqlca();

    ecpg_init_sqlca(sqlca);
    for (desc = get_descriptors(), prev = NULL; desc != NULL; prev = desc, desc = desc->next) {
        if (strcmp(name, desc->name) == 0) {
            if (prev != NULL)
                prev->next = desc->next;
            else
                set_descriptors(desc->next);
            descriptor_free(desc);
            return true;
        }
    }
    ecpg_raise(line, ECPG_UNKNOWN_DESCRIPTOR, ECPG_SQLSTATE_INVALID_SQL_DESCRIPTOR_NAME, name);
    return false;
}

#ifdef ENABLE_THREAD_SAFETY

/* Deallocate all descriptors in the list */
static void descriptor_deallocate_all(struct descriptor* list)
{
    while (list != NULL) {
        struct descriptor* next = list->next;

        descriptor_free(list);
        list = next;
    }
}
#endif /* ENABLE_THREAD_SAFETY */

bool ECPGallocate_desc(int line, const char* name)
{
    struct descriptor* newm;
    struct sqlca_t* sqlca = ECPGget_sqlca();

    ecpg_init_sqlca(sqlca);
    newm = (struct descriptor*)ecpg_alloc(sizeof(struct descriptor), line);
    if (newm == NULL)
        return false;
    newm->next = get_descriptors();
    newm->name = ecpg_alloc(strlen(name) + 1, line);
    if (newm->name == NULL) {
        ecpg_free(newm);
        return false;
    }
    newm->count = -1;
    newm->items = NULL;
    newm->result = (PGresult*)PQmakeEmptyPGresult(NULL, (ExecStatusType)0);
    if (newm->result == NULL) {
        ecpg_free(newm->name);
        ecpg_free(newm);
        ecpg_raise(line, ECPG_OUT_OF_MEMORY, ECPG_SQLSTATE_ECPG_OUT_OF_MEMORY, NULL);
        return false;
    }
    strcpy(newm->name, name);
    set_descriptors(newm);
    return true;
}

/* Find descriptor with name in the connection. */
struct descriptor* ecpg_find_desc(int line, const char* name)
{
    struct descriptor* desc;

    for (desc = get_descriptors(); desc != NULL; desc = desc->next) {
        if (strcmp(name, desc->name) == 0)
            return desc;
    }

    ecpg_raise(line, ECPG_UNKNOWN_DESCRIPTOR, ECPG_SQLSTATE_INVALID_SQL_DESCRIPTOR_NAME, name);
    return NULL; /* not found */
}

bool ECPGdescribe(int line, int compat, bool input, const char* connection_name, const char* stmt_name, ...)
{
    bool ret = false;
    struct connection* con;
    struct prepared_statement* prep;
    PGresult* res = NULL;
    va_list args;

    /* DESCRIBE INPUT is not yet supported */
    if (input) {
        ecpg_raise(line, ECPG_UNSUPPORTED, ECPG_SQLSTATE_ECPG_INTERNAL_ERROR, "DESCRIBE INPUT");
        return ret;
    }

    con = ecpg_get_connection(connection_name);
    if (con == NULL) {
        ecpg_raise(line,
            ECPG_NO_CONN,
            ECPG_SQLSTATE_CONNECTION_DOES_NOT_EXIST,
            connection_name != NULL ? connection_name : ecpg_gettext("NULL"));
        return ret;
    }
    prep = ecpg_find_prepared_statement(stmt_name, con, NULL);
    if (prep == NULL) {
        ecpg_raise(line, ECPG_INVALID_STMT, ECPG_SQLSTATE_INVALID_SQL_STATEMENT_NAME, stmt_name);
        return ret;
    }

    va_start(args, stmt_name);

    for (;;) {
        enum ECPGttype type;
        void* ptr = NULL;

        /* variable type */
        type = (ECPGttype)va_arg(args, int);

        if (type == ECPGt_EORT)
            break;

        /* rest of variable parameters */
        ptr = va_arg(args, void*);
        (void)va_arg(args, long); /* skip args */
        (void)va_arg(args, long);
        (void)va_arg(args, long);

        /* variable indicator */
        (void)va_arg(args, int);
        (void)va_arg(args, void*); /* skip args */
        (void)va_arg(args, long);
        (void)va_arg(args, long);
        (void)va_arg(args, long);

        switch (type) {
            case ECPGt_descriptor: {
                char* name = (char*)ptr;
                struct descriptor* desc = ecpg_find_desc(line, name);

                if (desc == NULL)
                    break;

                res = PQdescribePrepared(con->connection, stmt_name);
                if (!ecpg_check_PQresult(res, line, con->connection, (COMPAT_MODE)compat))
                    break;

                if (desc->result != NULL)
                    PQclear(desc->result);

                desc->result = res;
                ret = true;
                break;
            }
            case ECPGt_sqlda: {
                if (INFORMIX_MODE(compat)) {
                    struct sqlda_compat** _sqlda = (sqlda_compat**)ptr;
                    struct sqlda_compat* sqlda;

                    res = PQdescribePrepared(con->connection, stmt_name);
                    if (!ecpg_check_PQresult(res, line, con->connection, (COMPAT_MODE)compat))
                        break;

                    sqlda = ecpg_build_compat_sqlda(line, res, -1, (COMPAT_MODE)compat);
                    if (sqlda != NULL) {
                        struct sqlda_compat* sqlda_old = *_sqlda;
                        struct sqlda_compat* sqlda_old1;

                        while (sqlda_old != NULL) {
                            sqlda_old1 = sqlda_old->desc_next;
                            free(sqlda_old);
                            sqlda_old = sqlda_old1;
                        }

                        *_sqlda = sqlda;
                        ret = true;
                    }

                    PQclear(res);
                } else {
                    struct sqlda_struct** _sqlda = (sqlda_struct**)ptr;
                    struct sqlda_struct* sqlda;

                    res = PQdescribePrepared(con->connection, stmt_name);
                    if (!ecpg_check_PQresult(res, line, con->connection, (COMPAT_MODE)compat))
                        break;

                    sqlda = ecpg_build_native_sqlda(line, res, -1, (COMPAT_MODE)compat);
                    if (sqlda != NULL) {
                        struct sqlda_struct* sqlda_old = *_sqlda;
                        struct sqlda_struct* sqlda_old1;

                        while (sqlda_old != NULL) {
                            sqlda_old1 = sqlda_old->desc_next;
                            free(sqlda_old);
                            sqlda_old = sqlda_old1;
                        }

                        *_sqlda = sqlda;
                        ret = true;
                    }

                    PQclear(res);
                }
                break;
            }
            default:
                /* nothing else may come */
                ;
        }
    }

    va_end(args);

    return ret;
}