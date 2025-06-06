/******************************************************************************
 *
 * Component: OGDI Driver Support Library
 * Purpose: Generic SQL WHERE Expression Evaluator Declarations.
 * Author: Frank Warmerdam <warmerdam@pobox.com>
 *
 ******************************************************************************
 * Copyright (C) 2001 Information Interoperability Institute (3i)
 * Copyright (c) 2010-2013, Even Rouault <even dot rouault at spatialys.com>
 * Permission to use, copy, modify and distribute this software and
 * its documentation for any purpose and without fee is hereby granted,
 * provided that the above copyright notice appear in all copies, that
 * both the copyright notice and this permission notice appear in
 * supporting documentation, and that the name of 3i not be used
 * in advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  3i makes no
 * representations about the suitability of this software for any purpose.
 * It is provided "as is" without express or implied warranty.
 ****************************************************************************/

#ifndef SWQ_H_INCLUDED_
#define SWQ_H_INCLUDED_

#ifndef DOXYGEN_SKIP

#include "cpl_conv.h"
#include "cpl_string.h"
#include "ogr_core.h"

#include <list>
#include <map>
#include <vector>
#include <set>

#if defined(_WIN32) && !defined(strcasecmp)
#define strcasecmp stricmp
#endif

// Used for swq_summary.oSetDistinctValues and oVectorDistinctValues
#define SZ_OGR_NULL "__OGR_NULL__"

typedef enum
{
    SWQ_OR,
    SWQ_AND,
    SWQ_NOT,
    SWQ_EQ,
    SWQ_NE,
    SWQ_GE,
    SWQ_LE,
    SWQ_LT,
    SWQ_GT,
    SWQ_LIKE,
    SWQ_ILIKE,
    SWQ_ISNULL,
    SWQ_IN,
    SWQ_BETWEEN,
    SWQ_ADD,
    SWQ_SUBTRACT,
    SWQ_MULTIPLY,
    SWQ_DIVIDE,
    SWQ_MODULUS,
    SWQ_CONCAT,
    SWQ_SUBSTR,
    SWQ_HSTORE_GET_VALUE,

    SWQ_AVG,
    SWQ_AGGREGATE_BEGIN = SWQ_AVG,
    SWQ_MIN,
    SWQ_MAX,
    SWQ_COUNT,
    SWQ_SUM,
    SWQ_STDDEV_POP,
    SWQ_STDDEV_SAMP,
    SWQ_AGGREGATE_END = SWQ_STDDEV_SAMP,

    SWQ_CAST,
    SWQ_CUSTOM_FUNC,  /* only if parsing done in bAcceptCustomFuncs mode */
    SWQ_ARGUMENT_LIST /* temporary value only set during parsing and replaced by
                         something else at the end */
} swq_op;

typedef enum
{
    SWQ_INTEGER,
    SWQ_INTEGER64,
    SWQ_FLOAT,
    SWQ_STRING,
    SWQ_BOOLEAN,    // integer
    SWQ_DATE,       // string
    SWQ_TIME,       // string
    SWQ_TIMESTAMP,  // string
    SWQ_GEOMETRY,
    SWQ_NULL,
    SWQ_OTHER,
    SWQ_ERROR
} swq_field_type;

#define SWQ_IS_INTEGER(x) ((x) == SWQ_INTEGER || (x) == SWQ_INTEGER64)

typedef enum
{
    SNT_CONSTANT,
    SNT_COLUMN,
    SNT_OPERATION
} swq_node_type;

class swq_field_list;
class swq_expr_node;
class swq_select;
class OGRGeometry;

struct CPL_UNSTABLE_API swq_evaluation_context
{
    bool bUTF8Strings = false;
};

typedef swq_expr_node *(*swq_field_fetcher)(swq_expr_node *op,
                                            void *record_handle);
typedef swq_expr_node *(*swq_op_evaluator)(
    swq_expr_node *op, swq_expr_node **sub_field_values,
    const swq_evaluation_context &sContext);
typedef swq_field_type (*swq_op_checker)(
    swq_expr_node *op, int bAllowMismatchTypeOnFieldComparison);

class swq_custom_func_registrar;

class CPL_UNSTABLE_API swq_expr_node
{
    swq_expr_node *Evaluate(swq_field_fetcher pfnFetcher, void *record,
                            const swq_evaluation_context &sContext,
                            int nRecLevel);
    void reset();

  public:
    swq_expr_node();
    swq_expr_node(const swq_expr_node &);
    swq_expr_node(swq_expr_node &&);

    swq_expr_node &operator=(const swq_expr_node &);
    swq_expr_node &operator=(swq_expr_node &&);

    bool operator==(const swq_expr_node &) const;

    explicit swq_expr_node(const char *);
    explicit swq_expr_node(int);
    explicit swq_expr_node(GIntBig);
    explicit swq_expr_node(double);
    explicit swq_expr_node(OGRGeometry *);
    explicit swq_expr_node(swq_op);

    ~swq_expr_node();

    void MarkAsTimestamp();
    CPLString UnparseOperationFromUnparsedSubExpr(char **apszSubExpr);
    char *Unparse(swq_field_list *, char chColumnQuote);
    void Dump(FILE *fp, int depth);
    swq_field_type Check(swq_field_list *, int bAllowFieldsInSecondaryTables,
                         int bAllowMismatchTypeOnFieldComparison,
                         swq_custom_func_registrar *poCustomFuncRegistrar);
    swq_expr_node *Evaluate(swq_field_fetcher pfnFetcher, void *record,
                            const swq_evaluation_context &sContext);
    swq_expr_node *Clone();

    void ReplaceBetweenByGEAndLERecurse();
    void ReplaceInByOrRecurse();
    void PushNotOperationDownToStack();

    void RebalanceAndOr();

    bool HasReachedMaxDepth() const;

    swq_node_type eNodeType = SNT_CONSTANT;
    swq_field_type field_type = SWQ_INTEGER;

    /* only for SNT_OPERATION */
    void PushSubExpression(swq_expr_node *);
    void ReverseSubExpressions();
    swq_op nOperation = SWQ_OR;
    int nSubExprCount = 0;
    swq_expr_node **papoSubExpr = nullptr;

    /* only for SNT_COLUMN */
    int field_index = 0;
    int table_index = 0;
    char *table_name = nullptr;

    /* only for SNT_CONSTANT */
    int is_null = false;
    int64_t int_value = 0;
    double float_value = 0.0;
    OGRGeometry *geometry_value = nullptr;

    /* shared by SNT_COLUMN, SNT_CONSTANT and also possibly SNT_OPERATION when
     */
    /* nOperation == SWQ_CUSTOM_FUNC */
    char *string_value = nullptr; /* column name when SNT_COLUMN */

    // May be transiently used by swq_parser.h, but should not be relied upon
    // after parsing. swq_col_def.bHidden captures it afterwards.
    bool bHidden = false;

    // Recursive depth of this expression, taking into account papoSubExpr.
    int nDepth = 1;

    static CPLString QuoteIfNecessary(const CPLString &, char chQuote = '\'');
    static CPLString Quote(const CPLString &, char chQuote = '\'');
};

typedef struct
{
    const char *pszName;
    swq_op eOperation;
    swq_op_evaluator pfnEvaluator;
    swq_op_checker pfnChecker;
} swq_operation;

class CPL_UNSTABLE_API swq_op_registrar
{
  public:
    static const swq_operation *GetOperator(const char *);
    static const swq_operation *GetOperator(swq_op eOperation);
};

class CPL_UNSTABLE_API swq_custom_func_registrar
{
  public:
    virtual ~swq_custom_func_registrar();

    virtual const swq_operation *GetOperator(const char *) = 0;
};

typedef struct
{
    char *data_source;
    char *table_name;
    char *table_alias;
} swq_table_def;

class CPL_UNSTABLE_API swq_field_list
{
  public:
    int count;
    char **names;
    swq_field_type *types;
    int *table_ids;
    int *ids;

    int table_count;
    swq_table_def *table_defs;
};

class CPL_UNSTABLE_API swq_parse_context
{
  public:
    swq_parse_context()
        : nStartToken(0), pszInput(nullptr), pszNext(nullptr),
          pszLastValid(nullptr), bAcceptCustomFuncs(FALSE), poRoot(nullptr),
          poCurSelect(nullptr)
    {
    }

    int nStartToken;
    const char *pszInput;
    const char *pszNext;
    const char *pszLastValid;
    int bAcceptCustomFuncs;

    swq_expr_node *poRoot;

    swq_select *poCurSelect;
};

/* Compile an SQL WHERE clause into an internal form.  The field_list is
** the list of fields in the target 'table', used to render where into
** field numbers instead of names.
*/
int CPL_UNSTABLE_API swqparse(swq_parse_context *context);
int CPL_UNSTABLE_API swqlex(swq_expr_node **ppNode, swq_parse_context *context);
void CPL_UNSTABLE_API swqerror(swq_parse_context *context, const char *msg);

int CPL_UNSTABLE_API swq_identify_field(const char *table_name,
                                        const char *token,
                                        swq_field_list *field_list,
                                        swq_field_type *this_type,
                                        int *table_id);

CPLErr CPL_UNSTABLE_API
swq_expr_compile(const char *where_clause, int field_count, char **field_list,
                 swq_field_type *field_types, int bCheck,
                 swq_custom_func_registrar *poCustomFuncRegistrar,
                 swq_expr_node **expr_root);

CPLErr CPL_UNSTABLE_API
swq_expr_compile2(const char *where_clause, swq_field_list *field_list,
                  int bCheck, swq_custom_func_registrar *poCustomFuncRegistrar,
                  swq_expr_node **expr_root);

/*
** Evaluation related.
*/
int CPL_UNSTABLE_API swq_test_like(const char *input, const char *pattern);

swq_expr_node CPL_UNSTABLE_API *
SWQGeneralEvaluator(swq_expr_node *, swq_expr_node **,
                    const swq_evaluation_context &sContext);
swq_field_type CPL_UNSTABLE_API
SWQGeneralChecker(swq_expr_node *node, int bAllowMismatchTypeOnFieldComparison);
swq_expr_node CPL_UNSTABLE_API *
SWQCastEvaluator(swq_expr_node *, swq_expr_node **,
                 const swq_evaluation_context &sContext);
swq_field_type CPL_UNSTABLE_API
SWQCastChecker(swq_expr_node *node, int bAllowMismatchTypeOnFieldComparison);
const char CPL_UNSTABLE_API *SWQFieldTypeToString(swq_field_type field_type);

/****************************************************************************/

#define SWQP_ALLOW_UNDEFINED_COL_FUNCS 0x01

#define SWQM_SUMMARY_RECORD 1
#define SWQM_RECORDSET 2
#define SWQM_DISTINCT_LIST 3

typedef enum
{
    SWQCF_NONE = 0,
    SWQCF_AVG = SWQ_AVG,
    SWQCF_MIN = SWQ_MIN,
    SWQCF_MAX = SWQ_MAX,
    SWQCF_COUNT = SWQ_COUNT,
    SWQCF_SUM = SWQ_SUM,
    SWQCF_STDDEV_POP = SWQ_STDDEV_POP,
    SWQCF_STDDEV_SAMP = SWQ_STDDEV_SAMP,
    SWQCF_CUSTOM
} swq_col_func;

typedef struct
{
    swq_col_func col_func;
    char *table_name;
    char *field_name;
    char *field_alias;
    int table_index;
    int field_index;
    swq_field_type field_type;
    swq_field_type target_type;
    OGRFieldSubType target_subtype;
    int field_length;
    int field_precision;
    int distinct_flag;
    bool bHidden;
    OGRwkbGeometryType eGeomType;
    int nSRID;
    swq_expr_node *expr;
} swq_col_def;

class CPL_UNSTABLE_API swq_summary
{
  public:
    struct Comparator
    {
        bool bSortAsc;
        swq_field_type eType;

        Comparator() : bSortAsc(true), eType(SWQ_STRING)
        {
        }

        bool operator()(const CPLString &, const CPLString &) const;
    };

    //! Return the sum, using Kahan-Babuska-Neumaier algorithm.
    // Cf cf KahanBabushkaNeumaierSum of https://en.wikipedia.org/wiki/Kahan_summation_algorithm#Further_enhancements
    double sum() const
    {
        return sum_only_finite_terms ? sum_acc + sum_correction : sum_acc;
    }

    GIntBig count = 0;

    std::vector<CPLString> oVectorDistinctValues{};
    std::set<CPLString, Comparator> oSetDistinctValues{};
    bool sum_only_finite_terms = true;
    // Sum accumulator. To get the accurate sum, use the sum() method
    double sum_acc = 0.0;
    // Sum correction term.
    double sum_correction = 0.0;
    double min = 0.0;
    double max = 0.0;

    // Welford's online algorithm for variance:
    // https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Welford's_online_algorithm
    double mean_for_variance = 0.0;
    double sq_dist_from_mean_acc = 0.0;  // "M2"

    CPLString osMin{};
    CPLString osMax{};
};

typedef struct
{
    char *table_name;
    char *field_name;
    int table_index;
    int field_index;
    int ascending_flag;
} swq_order_def;

typedef struct
{
    int secondary_table;
    swq_expr_node *poExpr;
} swq_join_def;

class CPL_UNSTABLE_API swq_select_parse_options
{
  public:
    swq_custom_func_registrar *poCustomFuncRegistrar;
    int bAllowFieldsInSecondaryTablesInWhere;
    int bAddSecondaryTablesGeometryFields;
    int bAlwaysPrefixWithTableName;
    int bAllowDistinctOnGeometryField;
    int bAllowDistinctOnMultipleFields;

    swq_select_parse_options()
        : poCustomFuncRegistrar(nullptr),
          bAllowFieldsInSecondaryTablesInWhere(FALSE),
          bAddSecondaryTablesGeometryFields(FALSE),
          bAlwaysPrefixWithTableName(FALSE),
          bAllowDistinctOnGeometryField(FALSE),
          bAllowDistinctOnMultipleFields(FALSE)
    {
    }
};

class CPL_UNSTABLE_API swq_select
{
    void postpreparse();

    CPL_DISALLOW_COPY_ASSIGN(swq_select)

  public:
    swq_select();
    ~swq_select();

    int query_mode = 0;

    char *raw_select = nullptr;

    int PushField(swq_expr_node *poExpr, const char *pszAlias,
                  bool distinct_flag, bool bHidden);

    int PushExcludeField(swq_expr_node *poExpr);

    int result_columns() const
    {
        return static_cast<int>(column_defs.size());
    }

    std::vector<swq_col_def> column_defs{};
    std::vector<swq_summary> column_summary{};

    int PushTableDef(const char *pszDataSource, const char *pszTableName,
                     const char *pszAlias);
    int table_count = 0;
    swq_table_def *table_defs = nullptr;

    void PushJoin(int iSecondaryTable, swq_expr_node *poExpr);
    int join_count = 0;
    swq_join_def *join_defs = nullptr;

    swq_expr_node *where_expr = nullptr;

    void PushOrderBy(const char *pszTableName, const char *pszFieldName,
                     int bAscending);
    int order_specs = 0;
    swq_order_def *order_defs = nullptr;

    void SetLimit(GIntBig nLimit);
    GIntBig limit = -1;

    void SetOffset(GIntBig nOffset);
    GIntBig offset = 0;

    swq_select *poOtherSelect = nullptr;
    void PushUnionAll(swq_select *poOtherSelectIn);

    CPLErr preparse(const char *select_statement,
                    int bAcceptCustomFuncs = FALSE);
    CPLErr expand_wildcard(swq_field_list *field_list,
                           int bAlwaysPrefixWithTableName);
    CPLErr parse(swq_field_list *field_list,
                 swq_select_parse_options *poParseOptions);

    char *Unparse();

    bool bExcludedGeometry = false;

  private:
    bool IsFieldExcluded(int src_index, const char *table, const char *field);

    // map of EXCLUDE columns keyed according to the index of the
    // asterisk with which it should be associated. key of -1 is
    // used for column lists that have not yet been associated with
    // an asterisk.
    std::map<int, std::list<swq_col_def>> m_exclude_fields{};
};

/* This method should generally be invoked with pszValue set, except when
 * called on a non-DISTINCT column definition of numeric type (SWQ_BOOLEAN,
 * SWQ_INTEGER, SWQ_INTEGER64, SWQ_FLOAT), in which case pdfValue should
 * rather be set.
 */
const char CPL_UNSTABLE_API *swq_select_summarize(swq_select *select_info,
                                                  int dest_column,
                                                  const char *pszValue,
                                                  const double *pdfValue);

int CPL_UNSTABLE_API swq_is_reserved_keyword(const char *pszStr);

char CPL_UNSTABLE_API *OGRHStoreGetValue(const char *pszHStore,
                                         const char *pszSearchedKey);

#ifdef GDAL_COMPILATION
void swq_fixup(swq_parse_context *psParseContext);
swq_expr_node *swq_create_and_or_or(swq_op op, swq_expr_node *left,
                                    swq_expr_node *right);
int swq_test_like(const char *input, const char *pattern, char chEscape,
                  bool insensitive, bool bUTF8Strings);
#endif

#endif /* #ifndef DOXYGEN_SKIP */

#endif /* def SWQ_H_INCLUDED_ */
