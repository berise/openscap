/*
 * xccdf_policy.c
 *
 * Copyright 2009 Red Hat Inc., Durham, North Carolina.
 * All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software 
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  Created on: Dec 16, 2009
 *      Author: David Niemoller
 *              Maros Barabas <mbarabas@redhat.com>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <math.h> /* For isnan() */
#include <time.h> /* For timestamps in rule results and TestResult */
#include <libgen.h>

#include "public/xccdf_policy.h"
#include "public/xccdf_benchmark.h"
#include "public/oscap_text.h"

#include "cpe_lang.h"

#include "oval_agent_api.h"

#include "item.h"
#include "common/list.h"
#include "common/_error.h"
#include "common/debug_priv.h"
#include "common/assume.h"

/**
 * Typedef of callback structure with system identificator, callback function and usr data (optional) 
 * On evaluation action will be selected checking system and appropriate callback registred by tool
 * for that system.
 */
typedef struct callback_t {

    char * system;                              ///< Identificator of checking engine
    int (*callback) (struct xccdf_policy *,                     // Policy model
                      const char *,                             // Rule ID
                      const char *,                             // Definition ID
                      const char *,                             // HREF ID
                      struct xccdf_value_binding_iterator *, // Value Bindings Iterator
                      struct xccdf_check_import_iterator *,  // Check imports for the checking engine to interpret
                      void *);                  ///< format of callback function 
    void * usr;                                 ///< User data structure
    xccdf_policy_engine_query_fn query_fn;      ///< query callback function

} callback;

/**
 * Typedef of callback structure with callback function and usr data (optional)
 * After rule evaluation action will be called the callback with user data.
 */
typedef struct callback_out_t {

    char * system;                              ///< Identificator of checking engine (output engine)
    int (*callback)(void*,void*);             	///< policy report callback {output,start}
    void * usr;                                 ///< User data structure

} callback_out;

/**
 * XCCDF policy model structure contains xccdf_benchmark as reference
 * to Benchmark element in XML file and list of policies that are
 * abstract structure of Profile element from benchmark file.
 */
struct xccdf_policy_model {

        struct xccdf_benchmark  * benchmark;    ///< Benchmark element (root element of XML file)
	struct oscap_list       * policies;     ///< List of xccdf_policy structures
        struct oscap_list       * callbacks;    ///< Callbacks for checking engines (see callback_t)
        struct oscap_list       * cpe_dicts; ///< All CPE dictionaries except the one embedded in XCCDF
	struct oscap_list       * cpe_lang_models; ///< All CPE lang models except the one embedded in XCCDF
        struct oscap_htable     * cpe_oval_sessions; ///< Caches CPE OVAL check results
};
/* Macros to generate iterators, getters and setters */
OSCAP_GETTER(struct xccdf_benchmark *, xccdf_policy_model, benchmark)
OSCAP_IGETINS_GEN(xccdf_policy, xccdf_policy_model, policies, policy)

/**
 * XCCDF policy structure is abstract (class) structure
 * of Profile element from benchmark.
 *
 * Structure contains rules and bound values to abstract
 * these lists from the benchmark file. Can be modified temporaly
 * so changes can be discarded or saved to the existing model.
 */
struct xccdf_policy {

        struct xccdf_policy_model   * model;    ///< XCCDF Policy model
        struct xccdf_profile        * profile;  ///< Profile structure (from benchmark)
        struct oscap_list           * selects;  ///< Selected rules and groups of profile
        struct oscap_list           * values;   ///< Bound values of profile
        struct oscap_list           * results;  ///< List of XCCDF results
        /* Private sector */
        struct oscap_htable         * ht_selects; ///< Hash table of selects
};

/* Macros to generate iterators, getters and setters */
OSCAP_GETTER(struct xccdf_policy_model *, xccdf_policy, model)
OSCAP_GETTER(struct xccdf_profile *, xccdf_policy, profile)
OSCAP_IGETINS(xccdf_select, xccdf_policy, selects, select)
OSCAP_IGETINS_GEN(xccdf_value_binding, xccdf_policy, values, value)
OSCAP_IGETINS(xccdf_result, xccdf_policy, results, result)

/**
 * XCCDF value binding structure is binding between Refine values, Set values, 
 * Value element and Check export element of benchmark. These structures are 
 * binded together for exporting values to checking engine.
 *
 * These bindings are set during the preprocessing of profile, when policies 
 * are beeing created.
 */
struct xccdf_value_binding {

        char * name;                ///< The name of OVAL Variable
        xccdf_value_type_t type;    ///< Type of Variable
        char * value;               ///< Value of variable
        char * setvalue;            ///< Set value if defined or NULL
        xccdf_operator_t operator;  ///< Operator of Value
};
/* Macros to generate iterators, getters and setters */
OSCAP_GETTER(xccdf_value_type_t, xccdf_value_binding, type)
OSCAP_GETTER(char *, xccdf_value_binding, name)
OSCAP_GETTER(char *, xccdf_value_binding, value)
OSCAP_GETTER(char *, xccdf_value_binding, setvalue)
OSCAP_GETTER(xccdf_operator_t, xccdf_value_binding, operator)

/**
 * XCCDF Default score structure represents Default XCCDF Score model
 * for each rule
 */
typedef struct xccdf_default_score {

        float score;
        float accumulator;
        float weight_score;
        int count;

} xccdf_default_score_t;

/**
 * XCCDF Flat score structure represents Flat XCCDF Score model
 * for each rule
 */
typedef struct xccdf_flat_score {

        float score;
        float weight;

} xccdf_flat_score_t;

/*==========================================================================*/
/* Declaration of static (private to this file) functions
 * These function shoud not be called from outside. For exporting these 
 * elements has to call parent element's 
 */
static struct xccdf_refine_rule * xccdf_policy_get_refine_rules_by_rule(struct xccdf_policy * policy, struct xccdf_item * item);

/**
 * Filter function returning true if the item is selected, false otherwise
 */
static bool xccdf_policy_filter_selected(void *item, void *policy)
{
        struct xccdf_benchmark * benchmark 
            = xccdf_policy_model_get_benchmark(xccdf_policy_get_model((struct xccdf_policy *) policy));

        struct xccdf_item * titem = xccdf_benchmark_get_item(benchmark, xccdf_select_get_item((struct xccdf_select *) item));
        if (titem == NULL) {
            oscap_dlprintf(DBG_E, "Item \"%s\" does not exist. Remove it from Profile !\n", xccdf_select_get_item((struct xccdf_select *) item));
            return false;
        }
	return ((xccdf_item_get_type(titem) == XCCDF_RULE) &&
		(xccdf_select_get_selected((struct xccdf_select *) item)));
}

/**
 * Filter function returning true if the rule match ruleid, false otherwise
 */
static bool xccdf_policy_filter_select(void *item, void *selectid)
{
	return strcmp(xccdf_select_get_item((struct xccdf_select *) item), (char *) selectid) == 0;
}

/**
 * Filter function returning true if given callback is for the given checking engine,
 * false otherwise.
 */
static bool
_xccdf_policy_filter_callback(callback *cb, const char *sysname)
{
	return oscap_strcmp(cb->system, sysname) == 0;
}

/**
 * Get callbacks that match sysname. Call this function to get iterator over list of callbacks
 * that have the same system name
 */
static struct oscap_iterator *
_xccdf_policy_get_callbacks_by_sysname(struct xccdf_policy * policy, const char * sysname)
{
	return oscap_iterator_new_filter( policy->model->callbacks, (oscap_filter_func) _xccdf_policy_filter_callback, (void *) sysname);
}

/**
 * Return true if given policy has the select, false otherwise.
 */
static bool xccdf_policy_has_select(struct xccdf_policy * policy, const char *item_id)
{
        __attribute__nonnull__(policy);
        
        struct xccdf_select_iterator    * sel_it;
        struct xccdf_select             * sel;

        sel_it = xccdf_policy_get_selects(policy);
        while (xccdf_select_iterator_has_more(sel_it)) {
                sel = xccdf_select_iterator_next(sel_it);
                if (!strcmp(xccdf_select_get_item(sel), item_id)) {
                    xccdf_select_iterator_free(sel_it);
                    return true;
                }
        }
        xccdf_select_iterator_free(sel_it);
        return false;
}

/**
 * Get last setvalue from policy that match specified id
 */
static struct xccdf_setvalue * xccdf_policy_get_setvalue(struct xccdf_policy * policy, const char * id)
{
    /* return NULL if id or policy is NULL but don't use
     * __attribute_not_null__ here, it will cause abort
     * which is not desired
     */
    if (id == NULL) return NULL;
    if (policy == NULL) return NULL;

    struct xccdf_setvalue_iterator  * s_value_it;
    struct xccdf_setvalue           * s_value;
    struct xccdf_profile            * profile = xccdf_policy_get_profile(policy);;

    /* If profile is NULL we don't have setvalue's
     * and we return NULL, otherwise we could cause SIGSEG
     * with accessing NULL structure
     */
    if (profile == NULL) return NULL;

    /* We need to return *LAST* setvalue in Profile.
     * and iterate to the end of setvalue list
     */
    struct xccdf_setvalue * final = NULL;
    s_value_it = xccdf_profile_get_setvalues(profile);
    while (xccdf_setvalue_iterator_has_more(s_value_it)) {
        s_value = xccdf_setvalue_iterator_next(s_value_it);
        if (!strcmp(id, xccdf_setvalue_get_item(s_value))) {
            final = s_value;
        }
    }
    xccdf_setvalue_iterator_free(s_value_it);

    return final;
}

static struct xccdf_refine_value * xccdf_policy_get_refine_value(struct xccdf_policy * policy, const char * id)
{
    /* return NULL if id or policy is NULL but don't use
     * __attribute_not_null__ here, it will cause abort
     * which is not desired
     */
    if (id == NULL) return NULL;
    if (policy == NULL) return NULL;

    struct xccdf_refine_value_iterator  * r_value_it;
    struct xccdf_refine_value           * r_value;
    struct xccdf_profile            * profile = xccdf_policy_get_profile(policy);;

    /* If profile is NULL we don't have setvalue's
     * and we return NULL, otherwise we could cause SIGSEG
     * with accessing NULL structure
     */
    if (profile == NULL) return NULL;

    /* We need to return *LAST* setvalue in Profile.
     * and iterate to the end of setvalue list
     */
    struct xccdf_refine_value * final = NULL;
    r_value_it = xccdf_profile_get_refine_values(profile);
    while (xccdf_refine_value_iterator_has_more(r_value_it)) {
        r_value = xccdf_refine_value_iterator_next(r_value_it);
        if (!strcmp(id, xccdf_refine_value_get_item(r_value))) {
            final = r_value;
        }
    }
    xccdf_refine_value_iterator_free(r_value_it);

    return final;
}

/**
 * Function resolves two operations:
 *  P - PASS
 *  F - FAIL
 *  U - UNKNOWN
 *  E - ERROR
 *  N - NOT CHECKED, NOT SELECTED, NOT APPLICABLE
 *
 * ***************************************************************
 * AND  P  F  U  E  N    OR  P  F  U  E  N         P  F  U  E  N *
 *   P  P  F  U  E  P     P  P  P  P  P  P    neg  F  P  U  E  N *
 *   F  F  F  F  F  F     F  P  F  U  E  F                       *
 *   U  U  F  U  U  U     U  P  U  U  U  U                       *
 *   E  E  F  U  E  E     E  P  E  U  E  E                       *
 *   N  P  F  U  E  N     N  P  F  U  E  N                       *
 * ***************************************************************
 *
 */

static xccdf_test_result_type_t _resolve_operation(int A, int B, xccdf_bool_operator_t oper)
{

    xccdf_test_result_type_t value = 0;

    static const xccdf_test_result_type_t RESULT_TABLE_AND[9][9] = {
        /*  P  F  E  U  N  K  S  I */
        {0, 0, 0, 0, 0, 0, 0, 0, 0},
	{0, 1, 2, 3, 4, 1, 1, 1, 1}, /* P (pass)*/
	{0, 2, 2, 2, 2, 2, 2, 2, 2}, /* F (fail) */
	{0, 4, 2, 4, 4, 4, 4, 4, 4}, /* E (error) */
	{0, 3, 2, 3, 4, 3, 3, 3, 3}, /* U (unknown) */
	{0, 1, 2, 3, 4, 5, 5, 5, 5}, /* N (notapplicable) */
	{0, 1, 2, 3, 4, 5, 6, 6, 6}, /* K (notchecked) */
	{0, 1, 2, 3, 4, 5, 6, 7, 7}, /* S (notselected) */
	{0, 1, 2, 3, 4, 5, 6, 7, 8}};/* I (informational) */

    static const xccdf_test_result_type_t RESULT_TABLE_OR[9][9] = {
        /*  P  F  E  U  N  K  S  I */
        {0, 0, 0, 0, 0, 0, 0, 0, 0},
	{0, 1, 1, 1, 1, 1, 1, 1, 1}, /* P (pass)*/
	{0, 1, 2, 3, 4, 2, 2, 2, 2}, /* F (fail) */
	{0, 1, 4, 4, 4, 4, 4, 4, 4}, /* E (error) */
	{0, 1, 3, 3, 4, 3, 3, 3, 3}, /* U (unknown) */
	{0, 1, 2, 3, 4, 5, 5, 5, 5}, /* N (notapplicable) */
	{0, 1, 2, 3, 4, 5, 6, 6, 6}, /* K (notchecked) */
	{0, 1, 2, 3, 4, 5, 6, 7, 7}, /* S (notselected) */
	{0, 1, 2, 3, 4, 5, 6, 7, 8}};/* I (informational) */

    /* No test result can end with 0
     */
    if ((A == 0) || (B == 0)
	|| A > XCCDF_RESULT_INFORMATIONAL || B > XCCDF_RESULT_INFORMATIONAL) {
	oscap_dlprintf(DBG_E, "Bad test results %d, %d.\n", A, B);
	return 0;
    }

    switch (oper) {
        case XCCDF_OPERATOR_AND: /* AND */
            value = (xccdf_test_result_type_t) RESULT_TABLE_AND[A][B];
            break;

        case XCCDF_OPERATOR_OR: /* OR */
            value = (xccdf_test_result_type_t) RESULT_TABLE_OR[A][B];
            break;
	default:
	    oscap_dlprintf(DBG_E, "Operation not supported.\n");
            return 0;
            break;
    }

    return value;
}

xccdf_test_result_type_t xccdf_test_result_resolve_and_operation(xccdf_test_result_type_t A, xccdf_test_result_type_t B)
{
	return _resolve_operation((int)A, (int)B, XCCDF_OPERATOR_AND);
}

/**
 * Handle the negation="true" paramter of xccdf:complex-check.
 * Shall be run only once per a complex-check.
 */
static xccdf_test_result_type_t _resolve_negate(xccdf_test_result_type_t value, const struct xccdf_check *check)
{
    if (xccdf_check_get_negate(check)) {
        if (value == XCCDF_RESULT_PASS) return XCCDF_RESULT_FAIL;
        else if (value == XCCDF_RESULT_FAIL) return XCCDF_RESULT_PASS;
    }
    return value;
}

/**
 * Resolve the xccdf item. Parameter selected indicates parents selection attribute
 * It is used to decide the final selection attribute of item
 *
 * If parent's group of rule item has attribute selected = FALSE, It should not be processed, neither
 * its children. This selected attribute is iterativly passed to each child of group to set their selected
 * attribute to false no matter of profile setting.
 */
static void xccdf_policy_resolve_item(struct xccdf_policy * policy, struct xccdf_item * item, bool selected)
{
        __attribute__nonnull__(policy);
        __attribute__nonnull__(item);

        struct xccdf_item_iterator  * child_it;
        struct xccdf_item           * child;
        struct xccdf_select         * sel;

        xccdf_type_t itype = xccdf_item_get_type(item);

        switch (itype) {

            case XCCDF_RULE:{
                /* We don't have this rule's selector in policy yet - make new selector, set the rule attribute
                 * "selected" to the value of "Is parent selected and this rule is selected by default"
                 */
                if (!xccdf_policy_has_select(policy, xccdf_rule_get_id((const struct xccdf_rule *)item))) {
                    sel = xccdf_select_new();
                    xccdf_select_set_selected(sel, selected & xccdf_rule_get_selected((const struct xccdf_rule *)item));
                    xccdf_select_set_item(sel, xccdf_rule_get_id((const struct xccdf_rule *)item));
                    oscap_list_add(policy->selects, sel);
                /* The rule's selector is already in policy and we need to change "selected" attribute to the desired
                 * value (see comment above).
                 */
                } else {
                    sel = xccdf_policy_get_select_by_id(policy, xccdf_rule_get_id((const struct xccdf_rule *)item));
                    xccdf_select_set_selected(sel, selected & xccdf_select_get_selected(sel));
                }
            } break;

            case XCCDF_GROUP:{
                /* It is a group, if selected is 0, then group will be not processed, but if selected is 1 or nothing 
                 * by default, group will be processed and we need to iterate through list of rules
                 */
                if (selected) {
                    if (xccdf_policy_has_select(policy, xccdf_item_get_id(item))) {
                        sel = xccdf_policy_get_select_by_id(policy, xccdf_item_get_id(item));
                        selected = xccdf_select_get_selected(sel);
                    } else if (xccdf_group_get_selected((const struct xccdf_group *)item)){ /* it's selected */
                        selected = true;
                    } else selected = false;
                }

                child_it = xccdf_group_get_content((const struct xccdf_group *)item);
                while (xccdf_item_iterator_has_more(child_it)) {
                        child = xccdf_item_iterator_next(child_it);
                        xccdf_policy_resolve_item(policy, child, selected);
                }
                xccdf_item_iterator_free(child_it);
            } break;
            
            default: 
              /* TODO: set warning bad argument and return ? */
              break;

        } 
}

/**
 * Evaluate the policy check with given checking system
 */
static int
xccdf_policy_evaluate_cb(struct xccdf_policy * policy, const char * sysname, const char * content, const char * href,
        const char * rule_id, struct oscap_list * bindings, struct xccdf_check_import_iterator * check_import_it)
{
    xccdf_test_result_type_t retval = XCCDF_RESULT_NOT_CHECKED;
    struct oscap_iterator * cb_it = _xccdf_policy_get_callbacks_by_sysname(policy, sysname);
    while (oscap_iterator_has_more(cb_it)) {
        callback * cb = (callback *) oscap_iterator_next(cb_it);
        if (cb == NULL) { /* No callback found - checking system not registered */
            oscap_seterr(OSCAP_EFAMILY_XCCDF, "Unknown callback for given checking system. Set callback first");
            oscap_iterator_free(cb_it);
            return XCCDF_RESULT_NOT_CHECKED;
        }

        struct xccdf_value_binding_iterator * binding_it = (struct xccdf_value_binding_iterator *) oscap_iterator_new(bindings);

        retval = cb->callback(policy, rule_id, content, href, binding_it, check_import_it, cb->usr);

        if (binding_it != NULL) xccdf_value_binding_iterator_free(binding_it);
        if (retval != XCCDF_RESULT_NOT_CHECKED) break;
    }
    oscap_iterator_free(cb_it);

    return retval;
}

/**
 * Find all posible names for given check-content-ref/@href, considering also the check/@system.
 * This is usefull for multi-check="true" feature.
 * @return list of names (even empty) if the given href found, NULL otherwise.
 */
static struct oscap_stringlist *
_xccdf_policy_get_namesfor_href(struct xccdf_policy *policy, const char *sysname, const char *href)
{
	struct oscap_iterator *cb_it = _xccdf_policy_get_callbacks_by_sysname(policy, sysname);
	struct oscap_stringlist *result = NULL;
	while (oscap_iterator_has_more(cb_it) && result == NULL) {
		callback *cb = (callback *) oscap_iterator_next(cb_it);
		if (cb == NULL)
			break;
		if (cb->query_fn == NULL)
			continue;
		result = (struct oscap_stringlist *) cb->query_fn(cb->usr, POLICY_ENGINE_QUERY_NAMES_FOR_HREF, (void *)href);
	}
	oscap_iterator_free(cb_it);
	return result;
}

static int xccdf_policy_report_cb(struct xccdf_policy * policy, const char * sysname, void *rule)
{
    int retval = 0;
    struct oscap_iterator * cb_it = _xccdf_policy_get_callbacks_by_sysname(policy, sysname);
    while (oscap_iterator_has_more(cb_it)) {
        callback_out * cb = (callback_out *) oscap_iterator_next(cb_it);

        /* Report */
	retval = cb->callback(rule, cb->usr);

        /* We still want to stop evaluation if user cancel it
         * TODO: We should have a way to stop evaluation of current item
         */
        if (retval != 0) break;
    }
    oscap_iterator_free(cb_it);

    return retval;
}

static struct oscap_list * xccdf_policy_check_get_value_bindings(struct xccdf_policy * policy, struct xccdf_check_export_iterator * check_it)
{
        __attribute__nonnull__(check_it);

        struct xccdf_check_export   * check;
        struct xccdf_value          * value;
        struct xccdf_benchmark      * benchmark;
        struct xccdf_value_binding  * binding;
        struct xccdf_policy_model   * model;
        struct xccdf_refine_value   * r_value;
        struct xccdf_setvalue       * s_value;
        struct oscap_list           * list = oscap_list_new();

        model = xccdf_policy_get_model(policy);
        benchmark = xccdf_policy_model_get_benchmark(model);

        while (xccdf_check_export_iterator_has_more(check_it)) {
            check = xccdf_check_export_iterator_next(check_it);

            binding = xccdf_value_binding_new();
            value = (struct xccdf_value *) xccdf_benchmark_get_item(benchmark, xccdf_check_export_get_value(check));
            if (value == NULL) {
                oscap_seterr(OSCAP_EFAMILY_XCCDF, "Value \"%s\" does not exist in benchmark", xccdf_check_export_get_value(check));
		oscap_list_free(list, oscap_free);
                return NULL;
            }

            /* Apply related setvalue from policy profile */
            s_value = xccdf_policy_get_setvalue(policy, xccdf_value_get_id(value));
            if (s_value != NULL) binding->setvalue = oscap_strdup((char *) xccdf_setvalue_get_value(s_value));

            /* Apply related refine value from policy profile */
            const char * selector = NULL;
            r_value = xccdf_policy_get_refine_value(policy, xccdf_value_get_id(value));
            if (r_value != NULL) {
                selector = xccdf_refine_value_get_selector(r_value);
                /* This refine value changes the value content */
                if (!isnan(xccdf_refine_value_get_oper(r_value))) {
                    binding->operator = xccdf_refine_value_get_oper(r_value);
                } else binding->operator = xccdf_value_get_oper(value);

            } else binding->operator = xccdf_value_get_oper(value);

            const struct xccdf_value_instance * val = xccdf_value_get_instance_by_selector(value, selector);
            if (val == NULL) {
                oscap_seterr(OSCAP_EFAMILY_XCCDF, "Attempt to get non-existent selector \"%s\" from variable \"%s\"", selector, xccdf_value_get_id(value));
		oscap_list_free(list, oscap_free);
                return NULL;
            }
            binding->value = oscap_strdup(xccdf_value_instance_get_value(val));
            binding->name = oscap_strdup((char *) xccdf_check_export_get_name(check));
            binding->type = xccdf_value_get_type(value);
            oscap_list_add(list, binding);
        }
        xccdf_check_export_iterator_free(check_it);

        return list;

}

/** 
 * Evaluate the XCCDF check. 
 * Name collision with xccdf_check -> changed to xccdf_policy_check 
 */
static int xccdf_policy_check_evaluate(struct xccdf_policy * policy, struct xccdf_check * check, char * rule_id)
{
    struct xccdf_check_iterator             * child_it;
    struct xccdf_check                      * child;
    struct xccdf_check_content_ref_iterator * content_it;
    struct xccdf_check_content_ref          * content;
    const char                              * content_name;
    const char                              * system_name;
    const char                              * href;
    struct oscap_list                       * bindings;
    int                                       ret = 0;
    int                                       ret2 = 0;

    /* At least one of check-content or check-content-ref must
        * appear in each check element. */
    if (xccdf_check_get_complex(check)) { /* we have complex subtree */
            child_it = xccdf_check_get_children(check);
            while (xccdf_check_iterator_has_more(child_it)) {
                child = xccdf_check_iterator_next(child_it);
                ret2 = xccdf_policy_check_evaluate(policy, child, rule_id);
                if (ret2 == -1) {
                    xccdf_check_iterator_free(child_it);
                    return -1;
		}
                if (ret == 0) ret = ret2;
                else {
                    ret = (int) _resolve_operation((xccdf_test_result_type_t) ret, (xccdf_test_result_type_t) ret2, xccdf_check_get_oper(check));
                }
            }
            xccdf_check_iterator_free(child_it);

    } else { /* This is <check> element */
            /* It depends on what operation we process - we do only Compliance Check */
            content_it = xccdf_check_get_content_refs(check);
            system_name = xccdf_check_get_system(check);
            bindings = xccdf_policy_check_get_value_bindings(policy, xccdf_check_get_exports(check));
            if (bindings == NULL) {
                xccdf_check_content_ref_iterator_free(content_it);
                return XCCDF_RESULT_UNKNOWN;
	    }
            while (xccdf_check_content_ref_iterator_has_more(content_it)) {
                content = xccdf_check_content_ref_iterator_next(content_it);
                content_name = xccdf_check_content_ref_get_name(content);
                href = xccdf_check_content_ref_get_href(content);

                struct xccdf_check_import_iterator * check_import_it = xccdf_check_get_imports(check);
                ret = xccdf_policy_evaluate_cb(policy, system_name, content_name, href, rule_id, bindings, check_import_it);
                // the evaluation has filled check imports at this point, we can simply free the iterator
                xccdf_check_import_iterator_free(check_import_it);

                // the content references are basically alternatives according to the specification
                // we should go through them in the order they are defined and we are done as soon
                // as we process one of them successfully
                if ((xccdf_test_result_type_t) ret != XCCDF_RESULT_NOT_CHECKED) {
			xccdf_check_inject_content_ref(check, content, NULL);
			break;
		}
            }
            xccdf_check_content_ref_iterator_free(content_it);
            oscap_list_free(bindings, (oscap_destruct_func) xccdf_value_binding_free);
    }
    /* Negate only once */
    ret = _resolve_negate(ret, check);
    return ret;
}

static inline bool
_xccdf_policy_is_engine_registered(struct xccdf_policy *policy, char *sysname)
{
	return oscap_list_contains(policy->model->callbacks, (void *) sysname, (oscap_cmp_func) _xccdf_policy_filter_callback);
}

static struct xccdf_check *
_xccdf_policy_rule_get_applicable_check(struct xccdf_policy *policy, struct xccdf_item *rule)
{
	// Citations inline come from NISTIR-7275r4.
	struct xccdf_check *result = NULL;
	{
		// If an <xccdf:Rule> contains an <xccdf:complex-check>, then the benchmark consumer MUST process
		// it and MUST ignore any <xccdf:check> elements that are also contained by the <xccdf:Rule>.
		struct xccdf_check_iterator *check_it = xccdf_rule_get_complex_checks(rule);
		if (xccdf_check_iterator_has_more(check_it))
			result = xccdf_check_iterator_next(check_it);
		xccdf_check_iterator_free(check_it);
	}
	if (result == NULL) {
		// Check Processing Algorithm -- Check.Initialize
		// Check Processing Algorithm -- Check.Selector
		struct xccdf_refine_rule *r_rule = xccdf_policy_get_refine_rules_by_rule(policy, rule);
		char *selector = (r_rule == NULL) ? NULL : (char *) xccdf_refine_rule_get_selector(r_rule);
		struct xccdf_check_iterator *candidate_it = xccdf_rule_get_checks_filtered(rule, selector);
		if (selector != NULL && !xccdf_check_iterator_has_more(candidate_it)) {
			xccdf_check_iterator_free(candidate_it);
			// If the refined selector does not match, checks without selector shall be used.
			candidate_it = xccdf_rule_get_checks_filtered(rule, NULL);
		}
		// Check Processing Algorithm -- Check.System
		while (xccdf_check_iterator_has_more(candidate_it)) {
			struct xccdf_check *check = xccdf_check_iterator_next(candidate_it);
			if (_xccdf_policy_is_engine_registered(policy, (char *) xccdf_check_get_system(check)))
				result = check;
		}
		xccdf_check_iterator_free(candidate_it);
	}
	// A tool processing the Benchmark for compliance checking must pick at most one check or
	// complex-check element to process for each Rule.
	return result;
}

static inline bool
_xccdf_policy_is_rule_selected(struct xccdf_policy *policy, const struct xccdf_rule *rule)
{
	const struct xccdf_select *sel = xccdf_policy_get_select_by_id(policy, xccdf_rule_get_id(rule));
	return xccdf_select_get_selected(sel);
}

static struct xccdf_rule_result * _xccdf_rule_result_new_from_rule(const struct xccdf_rule *rule,
								  struct xccdf_check *check,
								  xccdf_test_result_type_t eval_result,
								  const char *message)
{
	struct xccdf_rule_result *rule_ritem = xccdf_rule_result_new();

	/* --Set rule-- */
        xccdf_rule_result_set_result(rule_ritem, eval_result);
	xccdf_rule_result_set_idref(rule_ritem, xccdf_rule_get_id(rule));
	xccdf_rule_result_set_weight(rule_ritem, xccdf_item_get_weight((struct xccdf_item *) rule));
	xccdf_rule_result_set_version(rule_ritem, xccdf_rule_get_version(rule));
	xccdf_rule_result_set_severity(rule_ritem, xccdf_rule_get_severity(rule));
	xccdf_rule_result_set_role(rule_ritem, xccdf_rule_get_role(rule));
	xccdf_rule_result_set_time(rule_ritem, time(NULL));

	/* --Fix --*/
	struct xccdf_fix_iterator *fix_it = xccdf_rule_get_fixes(rule);
	while (xccdf_fix_iterator_has_more(fix_it)) {
		struct xccdf_fix *fix = xccdf_fix_iterator_next(fix_it);
		xccdf_rule_result_add_fix(rule_ritem, xccdf_fix_clone(fix));
	}
	xccdf_fix_iterator_free(fix_it);

	/* --Ident-- */
	struct xccdf_ident_iterator * ident_it = xccdf_rule_get_idents(rule);
	while (xccdf_ident_iterator_has_more(ident_it)){
		struct xccdf_ident * ident = xccdf_ident_iterator_next(ident_it);
		xccdf_rule_result_add_ident(rule_ritem, xccdf_ident_clone(ident));
	}
	xccdf_ident_iterator_free(ident_it);
	if (check != NULL)
		xccdf_rule_result_add_check(rule_ritem, check);

	if (message != NULL) {
		struct xccdf_message *msg = xccdf_message_new();
		assume_ex(xccdf_message_set_content(msg, message), rule_ritem);
		assume_ex(xccdf_message_set_severity(msg, XCCDF_MSG_INFO), rule_ritem);
		assume_ex(xccdf_rule_result_add_message(rule_ritem, msg), rule_ritem);
	}
	return rule_ritem;
}

static int _xccdf_policy_report_rule_result(struct xccdf_policy *policy,
					    struct xccdf_result *result,
					    const struct xccdf_rule *rule,
					    struct xccdf_check *check,
					    int res,
					    const char *message)
{
	struct xccdf_rule_result * rule_result = NULL;
	int ret=0;

	if (res == -1)
		return res;

	if (result != NULL) {
		/* Add result to policy */
		/* TODO: instance */
		rule_result = _xccdf_rule_result_new_from_rule(rule, check, res, message);
		xccdf_result_add_rule_result(result, rule_result);
	} else
		xccdf_check_free(check);

	ret = xccdf_policy_report_cb(policy, "urn:xccdf:system:callback:output", (void *) rule_result);
	return ret;
}

struct cpe_check_cb_usr
{
	struct xccdf_policy_model* model;
	struct cpe_dict_model* dict;
	struct cpe_lang_model* lang_model;
};

static bool _xccdf_policy_cpe_check_cb(const char* system, const char* href, const char* name, void* usr)
{
	// FIXME: Check that system is OVAL

	struct cpe_check_cb_usr* cb_usr = (struct cpe_check_cb_usr*)usr;

	struct xccdf_policy_model* model = cb_usr->model;
	struct cpe_dict_model* dict = cb_usr->dict;

	char* prefixed_href = NULL;

	if (cb_usr->dict != NULL)
	{
		// the href path is relative to the CPE dictionary, we need to figre out
		// a "prefixed path" to deal with the case where CPE dict is not in CWD
		const char* origin_file_c = cpe_dict_model_get_origin_file(dict);
		// we need to strdup because dirname potentially alters the string
		char* origin_file = oscap_strdup(origin_file_c ? origin_file_c : "");
		const char* prefix_dirname = dirname(origin_file);
		prefixed_href = oscap_sprintf("%s/%s", prefix_dirname, href);
		oscap_free(origin_file);
	}
	else if (cb_usr->lang_model != NULL)
	{
		// FIXME: STUB!
		prefixed_href = oscap_strdup(href);
	}

	struct oval_agent_session* session = (struct oval_agent_session*)oscap_htable_get(model->cpe_oval_sessions, prefixed_href);

	if (session == NULL)
	{
		struct oval_definition_model* oval_model = oval_definition_model_import(prefixed_href);
		if (oval_model == NULL)
		{
			oscap_seterr(OSCAP_EFAMILY_OSCAP, "Can't import OVAL definition model '%s' for CPE applicability checking", prefixed_href);
			oscap_free(prefixed_href);
			return false;
		}

		session = oval_agent_new_session(oval_model, prefixed_href);
		oscap_htable_add(model->cpe_oval_sessions, prefixed_href, session);
		oscap_free(prefixed_href);
	}

	oval_agent_eval_definition(session, name);
	oval_result_t result = OVAL_RESULT_NOT_EVALUATED;
	if (oval_agent_get_definition_result(session, name, &result) != 0)
	{
		// error message should already be set in the function
	}

	return result == OVAL_RESULT_TRUE;
}

static bool _xccdf_policy_cpe_dict_cb(struct cpe_name* name, void* usr)
{
	struct cpe_check_cb_usr* cb_usr = (struct cpe_check_cb_usr*)usr;

	// We have to check all CPE1 dicts in the model.
	// cb_usr->dict has nothing to do with this callback! Do NOT touch it here.

	struct xccdf_policy_model* model = cb_usr->model;
	struct xccdf_benchmark* benchmark = xccdf_policy_model_get_benchmark(model);

	bool ret = false;

	struct cpe_dict_model* embedded_dict = xccdf_benchmark_get_cpe_list(benchmark);
	if (embedded_dict != NULL) {
		ret = cpe_name_applicable_dict(name, embedded_dict, (cpe_check_fn) _xccdf_policy_cpe_check_cb, usr);
	}
	if (ret)
		return true;

	struct oscap_iterator* dicts = oscap_iterator_new(model->cpe_dicts);
	while (!ret && oscap_iterator_has_more(dicts)) {
		struct cpe_dict_model *dict = (struct cpe_dict_model*)oscap_iterator_next(dicts);
		ret = cpe_name_applicable_dict(name, dict, (cpe_check_fn) _xccdf_policy_cpe_check_cb, usr);
	}
	oscap_iterator_free(dicts);
	return ret;
}

static bool xccdf_policy_model_item_is_applicable_dict(struct xccdf_policy_model* model, struct cpe_dict_model* dict, struct xccdf_item* item)
{
	struct oscap_string_iterator* platforms = xccdf_item_get_platforms(item);

	// at this point we know that the item has 1 or more platforms specified
	bool ret = false;

	while (oscap_string_iterator_has_more(platforms))
	{
		const char* platform = oscap_string_iterator_next(platforms);
		// Platform could be a reference to CPE2 platform, skip the ones
		// that aren't valid CPE names.
		if (!cpe_name_check(platform))
			continue;

		struct cpe_name* name = cpe_name_new(platform);

		struct cpe_check_cb_usr* usr = oscap_alloc(sizeof(struct cpe_check_cb_usr));
		usr->model = model;
		usr->dict = dict;
		usr->lang_model = NULL;
		const bool applicable = cpe_name_applicable_dict(name, dict, (cpe_check_fn) _xccdf_policy_cpe_check_cb, usr);
		oscap_free(usr);

		cpe_name_free(name);

		if (applicable)
		{
			ret = true;
			break;
		}
	}
	oscap_string_iterator_free(platforms);

	return ret;
}

static bool xccdf_policy_model_item_is_applicable_lang_model(struct xccdf_policy_model* model, struct cpe_lang_model* lang_model, struct xccdf_item* item)
{
	struct oscap_string_iterator* platforms = xccdf_item_get_platforms(item);

	// at this point we know that the item has 1 or more platforms specified
	bool ret = false;

	while (oscap_string_iterator_has_more(platforms))
	{
		const char* platform = oscap_string_iterator_next(platforms);
		// Specification says that platform should begin with "#" if it is
		// a reference to a CPE2 platform. However content exists where this
		// is not strictly followed so we support both with and without "#"
		// references.

		const char* platform_shifted = platform;
		if (strlen(platform_shifted) >= 1 && *platform_shifted == '#')
		{
			// skip the "#" character
			platform_shifted++;
		}

		struct cpe_check_cb_usr* usr = oscap_alloc(sizeof(struct cpe_check_cb_usr));
		usr->model = model;
		usr->dict = NULL;
		usr->lang_model = lang_model;
		const bool applicable = cpe_platform_applicable_lang_model(platform_shifted, lang_model, (cpe_check_fn)_xccdf_policy_cpe_check_cb, (cpe_dict_fn)_xccdf_policy_cpe_dict_cb, usr);
		oscap_free(usr);

		if (applicable)
		{
			ret = true;
			break;
		}
	}
	oscap_string_iterator_free(platforms);

	return ret;
}

static bool xccdf_policy_model_item_is_applicable(struct xccdf_policy_model* model, struct xccdf_item* item)
{
	struct xccdf_benchmark* benchmark = xccdf_item_get_benchmark(item);

	struct xccdf_item* parent = xccdf_item_get_parent(item);
	if (!parent || xccdf_policy_model_item_is_applicable(model, parent))
	{
		// we have to check whether the item has any platforms at all, if it has none
		// it should be applicable to all platforms
		struct oscap_string_iterator* platforms = xccdf_item_get_platforms(item);
		bool has_platforms = oscap_string_iterator_has_more(platforms);
		oscap_string_iterator_free(platforms);

		bool ret = true;

		// while is used just because of the break "early outs"
		while (has_platforms)
		{
			ret = false;

			// We do not check whether the platform entries are valid platform refs
			// or CPE names. We let the policy_model methods do that instead.
			// Therefore we check all 4 (!) places where a platform may match.
			// CPE2 takes precedence over CPE1 in this implementation. This is not
			// dictated by the specification, it's an arbitrary choice.

			struct cpe_lang_model* embedded_lang_model = xccdf_benchmark_get_cpe_lang_model(benchmark);
			if (embedded_lang_model != NULL) {
				ret = xccdf_policy_model_item_is_applicable_lang_model(model, embedded_lang_model, item);
			}
			if (ret)
				break;

			struct oscap_iterator* lang_models = oscap_iterator_new(model->cpe_lang_models);
			while (!ret && oscap_iterator_has_more(lang_models)) {
				struct cpe_lang_model *lang_model = (struct cpe_lang_model*)oscap_iterator_next(lang_models);
				ret = xccdf_policy_model_item_is_applicable_lang_model(model, lang_model, item);
			}
			oscap_iterator_free(lang_models);
			if (ret)
				break;

			struct cpe_dict_model* embedded_dict = xccdf_benchmark_get_cpe_list(benchmark);
			if (embedded_dict != NULL) {
				ret = xccdf_policy_model_item_is_applicable_dict(model, embedded_dict, item);
			}
			if (ret)
				break;

			struct oscap_iterator* dicts = oscap_iterator_new(model->cpe_dicts);
			while (!ret && oscap_iterator_has_more(dicts)) {
				struct cpe_dict_model *dict = (struct cpe_dict_model*)oscap_iterator_next(dicts);
				ret = xccdf_policy_model_item_is_applicable_dict(model, dict, item);
			}
			oscap_iterator_free(dicts);
			break;
		}

		return ret;
	}
	else
	{
		// parent is not applicable
		return false;
	}
}

/**
 * Evaluate given check which is immediate child of the rule.
 * A possibe child checks will be evaluated by xccdf_policy_check_evaluate.
 * This duplication is needed to handle @multi-check correctly,
 * which is (in general) not predictable in any way.
 */
static inline int
_xccdf_policy_rule_evaluate(struct xccdf_policy * policy, const struct xccdf_rule *rule, struct xccdf_result *result)
{
	const bool is_selected = _xccdf_policy_is_rule_selected(policy, rule);
	const char *message = NULL;

	int report = xccdf_policy_report_cb(policy, "urn:xccdf:system:callback:start", (void *) rule);
	if (report)
		return report;

	if (!is_selected)
		return _xccdf_policy_report_rule_result(policy, result, rule, NULL, XCCDF_RESULT_NOT_SELECTED, NULL);

	const bool is_applicable = xccdf_policy_model_item_is_applicable(policy->model, (struct xccdf_item*)rule);
	if (!is_applicable)
		return _xccdf_policy_report_rule_result(policy, result, rule, NULL, XCCDF_RESULT_NOT_APPLICABLE, NULL);

	const struct xccdf_check *orig_check = _xccdf_policy_rule_get_applicable_check(policy, (struct xccdf_item *) rule);
	if (orig_check == NULL)
		// No candidate or applicable check found.
		return _xccdf_policy_report_rule_result(policy, result, rule, NULL, XCCDF_RESULT_NOT_CHECKED, "No candidate or applicable check found.");

	// we need to clone the check to avoid changing the original content
	struct xccdf_check *check = xccdf_check_clone(orig_check);
	if (xccdf_check_get_complex(check))
		return _xccdf_policy_report_rule_result(policy, result, rule, check, xccdf_policy_check_evaluate(policy, check, NULL), NULL);

	// Now we are evaluating single simple xccdf:check within xccdf:rule.
	// Since the fact that a check will yield multi-check is not predictable in general
	// we will evaluate the check here. (A link between rule and its only check is
	// somewhat tigher that the one between checks in complex-check tree).
	//
	// Important: if touching this code, please revisit also xccdf_policy_check_evaluate.
	const char *system_name = xccdf_check_get_system(check);
	struct oscap_list *bindings = xccdf_policy_check_get_value_bindings(policy, xccdf_check_get_exports(check));
	if (bindings == NULL)
		return _xccdf_policy_report_rule_result(policy, result, rule, check, XCCDF_RESULT_UNKNOWN, "Value bindings not found.");


	struct xccdf_check_content_ref_iterator *content_it = xccdf_check_get_content_refs(check);
	struct xccdf_check_content_ref *content;
	const char *content_name;
	const char *href;
	int ret;
	while (xccdf_check_content_ref_iterator_has_more(content_it)) {
		message = NULL;
		content = xccdf_check_content_ref_iterator_next(content_it);
		content_name = xccdf_check_content_ref_get_name(content);
		href = xccdf_check_content_ref_get_href(content);

		if (content_name == NULL && xccdf_check_get_multicheck(check)) {
			// parent element is Rule, @multi-check is required
			struct oscap_stringlist *names = _xccdf_policy_get_namesfor_href(policy, system_name, href);
			if (names != NULL) {
				// multi-check is supported by checking-engine
				struct oscap_string_iterator *name_it = oscap_stringlist_get_strings(names);
				if (!oscap_string_iterator_has_more(name_it)) {
					// Super special case when oval file contains no definitions
					// thus multi-check shall yield zero rule-results.
					report = _xccdf_policy_report_rule_result(policy, result, rule, check, XCCDF_RESULT_UNKNOWN, "No definitions found for @multi-check.");
					oscap_string_iterator_free(name_it);
					oscap_stringlist_free(names);
					xccdf_check_content_ref_iterator_free(content_it);
					oscap_list_free(bindings, (oscap_destruct_func) xccdf_value_binding_free);
					return report;
				}
				while (oscap_string_iterator_has_more(name_it)) {
					const char *name = oscap_string_iterator_next(name_it);
					struct xccdf_check *cloned_check = xccdf_check_clone(check);
					xccdf_check_inject_content_ref(cloned_check, content, name);
					int inner_ret = xccdf_policy_check_evaluate(policy, cloned_check, NULL);
					if (inner_ret == -1) {
						xccdf_check_free(cloned_check);
						report = inner_ret;
						break;
					}
					if ((report = _xccdf_policy_report_rule_result(policy, result, rule, cloned_check, inner_ret, NULL)) != 0)
						break;
					if (oscap_string_iterator_has_more(name_it))
						if ((report = xccdf_policy_report_cb(policy, "urn:xccdf:system:callback:start", (void *) rule)) != 0)
							break;
				}
				oscap_string_iterator_free(name_it);
				oscap_stringlist_free(names);
				xccdf_check_content_ref_iterator_free(content_it);
				oscap_list_free(bindings, (oscap_destruct_func) xccdf_value_binding_free);
				xccdf_check_free(check);
				return report;
			}
			else
				message = "Checking engine does not support multi-check; falling back to multi-check='false'";
		}

		struct xccdf_check_import_iterator *check_import_it = xccdf_check_get_imports(check);
		ret = xccdf_policy_evaluate_cb(policy, system_name, content_name, href, NULL, bindings, check_import_it);
		// the evaluation has filled check imports at this point, we can simply free the iterator
		xccdf_check_import_iterator_free(check_import_it);

		// the content references are basically alternatives according to the specification
		// we should go through them in the order they are defined and we are done as soon
		// as we process one of them successfully
		if ((xccdf_test_result_type_t) ret != XCCDF_RESULT_NOT_CHECKED) {
			xccdf_check_inject_content_ref(check, content, NULL);
			break;
		}
	}
	if ((xccdf_test_result_type_t) ret == XCCDF_RESULT_NOT_CHECKED)
		message = "None of the check-content-ref elements was resolvable.";
	xccdf_check_content_ref_iterator_free(content_it);
	oscap_list_free(bindings, (oscap_destruct_func) xccdf_value_binding_free);
	/* Negate only once */
	ret = _resolve_negate(ret, check);
	return _xccdf_policy_report_rule_result(policy, result, rule, check, ret, message);
}

/** 
 * Evaluate the XCCDF item. If it is group, start recursive cycle, otherwise get XCCDF check
 * and evaluate it.
 * Name collision with xccdf_item -> changed to xccdf_policy_item 
 */
static int xccdf_policy_item_evaluate(struct xccdf_policy * policy, struct xccdf_item * item, struct xccdf_result * result)
{
    struct xccdf_item_iterator      * child_it;
    struct xccdf_item               * child;
    int                               ret = XCCDF_RESULT_UNKNOWN;

    xccdf_type_t itype = xccdf_item_get_type(item);

    switch (itype) {
        case XCCDF_RULE:{
			return _xccdf_policy_rule_evaluate(policy, (struct xccdf_rule *) item, result);
        } break;

        case XCCDF_GROUP:{
			child_it = xccdf_group_get_content((const struct xccdf_group *)item);
			while (xccdf_item_iterator_has_more(child_it)) {
				child = xccdf_item_iterator_next(child_it);
				ret = xccdf_policy_item_evaluate(policy, child, result);
				if (ret == -1) {
					xccdf_item_iterator_free(child_it);
					return -1;
				}

				if (ret == false) /* we got item that can't be processed */
					break;
			}
			xccdf_item_iterator_free(child_it);
        } break;
        
        default: 
                    /* TODO: set warning bad argument and return ? */
                    ret=false;
            break;
    } 

    return 0;
}

static struct xccdf_default_score * xccdf_item_get_default_score(struct xccdf_item * item, struct xccdf_result * test_result)
{

	struct xccdf_default_score  * score;
	struct xccdf_default_score  * ch_score;
	struct xccdf_rule_result    * rule_result;
	struct xccdf_item           * child;

	xccdf_type_t itype = xccdf_item_get_type(item);

	switch (itype) {
        case XCCDF_RULE: {
		/* Rule */
		const char *rule_id = xccdf_rule_get_id((const struct xccdf_rule *) item);
		rule_result = xccdf_result_get_rule_result_by_id(test_result, rule_id);
		if (rule_result == NULL) {
			dE("Rule result ID(%s) not fount", rule_id);
			return NULL;
		}

		/* Ignore these rules */
		if ((xccdf_rule_result_get_result(rule_result) == XCCDF_RESULT_NOT_SELECTED) ||
		    (xccdf_rule_result_get_result(rule_result) == XCCDF_RESULT_NOT_APPLICABLE) ||
		    (xccdf_rule_result_get_result(rule_result) == XCCDF_RESULT_INFORMATIONAL) ||
		    (xccdf_rule_result_get_result(rule_result) == XCCDF_RESULT_NOT_CHECKED))
			    return NULL;

		score = oscap_alloc(sizeof(struct xccdf_default_score));

		/* Count with this rule */
		score->count = 1;

		/* If the test result is 'pass', assign the node a score of 100, otherwise assign a score of 0 */
		if ((xccdf_rule_result_get_result(rule_result) == XCCDF_RESULT_PASS) ||
		    (xccdf_rule_result_get_result(rule_result) == XCCDF_RESULT_FIXED))
			score->score = 100.0;
		else
			score->score = 0.0;

		/* Default weight */
		score->weight_score = score->score * xccdf_item_get_weight(item);
        } break;

        case XCCDF_BENCHMARK:
        case XCCDF_GROUP: {
		/* Init */
		score = oscap_alloc(sizeof(struct xccdf_default_score));
		score->count = 0;
		score->score = 0.0;
		score->accumulator = 0.0;

		/* Recurse */
		struct xccdf_item_iterator * child_it;
		if (itype == XCCDF_GROUP)
			child_it = xccdf_group_get_content((const struct xccdf_group *)item);
		else
			child_it = xccdf_benchmark_get_content((const struct xccdf_benchmark *)item);

		while (xccdf_item_iterator_has_more(child_it)) {
			child = xccdf_item_iterator_next(child_it);
			ch_score = xccdf_item_get_default_score(child, test_result);

			if (ch_score == NULL) /* we got item that can't be processed */
				continue;

			if (ch_score->count == 0) {  /* we got item that has no selected items */
				oscap_free(ch_score);
				continue;
			}

			/* If child's count value is not 0, then add the child's wighted score to this node's score */
			score->score += ch_score->weight_score;
			score->count++;
			score->accumulator += xccdf_item_get_weight(child);

			oscap_free(ch_score);
		}

		/* Normalize */
		if (score->count && score->accumulator)
			score->score = score->score / score->accumulator;
		/* Default weight */
                score->weight_score = score->score * xccdf_item_get_weight(item);

		xccdf_item_iterator_free(child_it);
	} break;

        default: {
		dE("Unsupported item type: %d", itype);
		score=NULL;
	} break;

	} /* switch */

    return score;
}

static struct xccdf_flat_score * xccdf_item_get_flat_score(struct xccdf_item * item, struct xccdf_result * test_result, bool unweighted)
{
	struct xccdf_flat_score     * score;
	struct xccdf_flat_score     * ch_score;
	struct xccdf_rule_result    * rule_result;
	struct xccdf_item           * child;

	xccdf_type_t itype = xccdf_item_get_type(item);

	switch (itype) {
	case XCCDF_RULE:{
		/* Rule */
		const char *rule_id = xccdf_rule_get_id((const struct xccdf_rule *) item);
		rule_result = xccdf_result_get_rule_result_by_id(test_result, rule_id);
		if (rule_result == NULL) {
			dE("Rule result ID(%s) not fount", rule_id);
			return NULL;
		}

		/* Ignore these rules */
		if ((xccdf_rule_result_get_result(rule_result) == XCCDF_RESULT_NOT_SELECTED) ||
		    (xccdf_rule_result_get_result(rule_result) == XCCDF_RESULT_NOT_APPLICABLE) ||
		    (xccdf_rule_result_get_result(rule_result) == XCCDF_RESULT_INFORMATIONAL) ||
		    (xccdf_rule_result_get_result(rule_result) == XCCDF_RESULT_NOT_CHECKED))
			    return NULL;

		score = oscap_alloc(sizeof(struct xccdf_flat_score));

		/* max possible score = sum of weights*/
		if (unweighted)
			score->weight = 1.0;
		else score->weight =
			xccdf_item_get_weight(item);

		/* score = sum of weights of rules that pass */
		if ((xccdf_rule_result_get_result(rule_result) == XCCDF_RESULT_PASS) ||
		    (xccdf_rule_result_get_result(rule_result) == XCCDF_RESULT_FIXED)) {
			if (unweighted)
				score->score = 1.0;
			else
				score->score = xccdf_item_get_weight(item);
		} else
			score->score = 0.0;	/* fail */
	} break;
        case XCCDF_BENCHMARK:
        case XCCDF_GROUP:{
		/* Init */
		score = oscap_alloc(sizeof(struct xccdf_flat_score));
		score->score = 0;
		score->weight = 0.0;

		/* Recurse */
		struct xccdf_item_iterator * child_it;
		if (itype == XCCDF_GROUP)
			child_it = xccdf_group_get_content((const struct xccdf_group *)item);
		else
			child_it = xccdf_benchmark_get_content((const struct xccdf_benchmark *)item);

		while (xccdf_item_iterator_has_more(child_it)) {
			child = xccdf_item_iterator_next(child_it);
			ch_score = xccdf_item_get_flat_score(child, test_result, unweighted);

			if (ch_score == NULL) /* we got item that can't be processed */
				continue;

			if (ch_score->weight == 0) {  /* we got item that has no selected items */
				oscap_free(ch_score);
				continue;
			}

			/* If child's count value is not 0, then add the child's wighted score to this node's score */
			score->score += ch_score->score;
			score->weight += ch_score->weight;

			oscap_free(ch_score);
		}

		xccdf_item_iterator_free(child_it);
	} break;

        default: {
		dE("Unsupported item type: %d", itype);
		score=NULL;
	} break;

	} /* switch */

	return score;
}

struct oscap_file_entry {
	char* system_name;
	char* file;
};

struct oscap_file_entry *oscap_file_entry_new(void)
{
	struct oscap_file_entry *ret = oscap_calloc(1, sizeof(struct oscap_file_entry));
	return ret;
}

struct oscap_file_entry *oscap_file_entry_dup(struct oscap_file_entry * file_entry)
{
	struct oscap_file_entry *source = (struct oscap_file_entry *) file_entry;

	struct oscap_file_entry *ret = oscap_file_entry_new();
	ret->system_name = oscap_strdup(source->system_name);
	ret->file = oscap_strdup(source->file);

	return ret;
}

void oscap_file_entry_free(struct oscap_file_entry * entry)
{
	oscap_free(entry->system_name);
	oscap_free(entry->file);
	oscap_free(entry);
}

const char* oscap_file_entry_get_system(struct oscap_file_entry* entry)
{
	return entry->system_name;
}

const char* oscap_file_entry_get_file(struct oscap_file_entry* entry)
{
	return entry->file;
}

const struct oscap_file_entry *oscap_file_entry_iterator_next(struct oscap_file_entry_iterator *it)
{
	return oscap_iterator_next((struct oscap_iterator *)it);
}

bool oscap_file_entry_iterator_has_more(struct oscap_file_entry_iterator *it)
{
	return oscap_iterator_has_more((struct oscap_iterator *)it);
}

void oscap_file_entry_iterator_free(struct oscap_file_entry_iterator *it)
{
	oscap_iterator_free((struct oscap_iterator *)it);
}

void oscap_file_entry_iterator_reset(struct oscap_file_entry_iterator *it)
{
	oscap_iterator_reset((struct oscap_iterator *)it);
}

struct oscap_file_entry_list* oscap_file_entry_list_new(void)
{
	return (struct oscap_file_entry_list *) oscap_list_new();
}

static void oscap_file_entry_list_item_destructor(void* item)
{
	oscap_file_entry_free(item);
}

void oscap_file_entry_list_free(struct oscap_file_entry_list* list)
{
	oscap_list_free((struct oscap_list *) list, oscap_file_entry_list_item_destructor);
}

struct oscap_file_entry_iterator* oscap_file_entry_list_get_files(struct oscap_file_entry_list* list)
{
	return (struct oscap_file_entry_iterator *) oscap_iterator_new((struct oscap_list *) list);
}

static bool xccdf_file_entry_cmp_func(void *e1, void *e2)
{
	struct oscap_file_entry *entry1 = (struct oscap_file_entry *) e1;
	struct oscap_file_entry *entry2 = (struct oscap_file_entry *) e2;

	if (oscap_strcmp(entry1->system_name, entry2->system_name))
		return false;

	if (oscap_strcmp(entry1->file, entry2->file))
		return false;

	return true;
}

static struct oscap_file_entry_list * xccdf_check_get_systems_and_files(struct xccdf_check * check)
{
    struct xccdf_check_iterator             * child_it;
    struct xccdf_check                      * child;
    struct xccdf_check_content_ref_iterator * content_it;
    struct xccdf_check_content_ref          * content;
    struct oscap_file_entry					* file_entry;
    char									* href;
    char									* system_name;
    struct oscap_file_entry_list            * files;
    struct oscap_file_entry_list            * sub_files;

    system_name = (char *) xccdf_check_get_system(check);

    files = oscap_file_entry_list_new();
    if (xccdf_check_get_complex(check)) {
        child_it = xccdf_check_get_children(check);
        while (xccdf_check_iterator_has_more(child_it)) {
            child = xccdf_check_iterator_next(child_it);
            sub_files = xccdf_check_get_systems_and_files(child);

            struct oscap_file_entry_iterator *file_it = oscap_file_entry_list_get_files(sub_files);
            while (oscap_file_entry_iterator_has_more(file_it)) {
            	file_entry = (struct oscap_file_entry *) oscap_file_entry_iterator_next(file_it);

                if (!oscap_list_contains((struct oscap_list *)files, (void *) file_entry, (oscap_cmp_func) xccdf_file_entry_cmp_func))
                    oscap_list_add((struct oscap_list *)files, oscap_file_entry_dup(file_entry));
            }
            oscap_file_entry_iterator_free(file_it);
            oscap_file_entry_list_free(sub_files);
        }
        xccdf_check_iterator_free(child_it);
    } else {
        content_it = xccdf_check_get_content_refs(check);
        while (xccdf_check_content_ref_iterator_has_more(content_it)) {
            content = xccdf_check_content_ref_iterator_next(content_it);
            href = (char *) xccdf_check_content_ref_get_href(content);

            file_entry = (struct oscap_file_entry *) oscap_file_entry_new();
            file_entry->system_name = oscap_strdup(system_name);
            file_entry->file = oscap_strdup(href);

            if (!oscap_list_contains((struct oscap_list *) files, file_entry, (oscap_cmp_func) xccdf_file_entry_cmp_func))
                oscap_list_add((struct oscap_list *) files, (void *) file_entry);
            else
            	oscap_file_entry_free((void *) file_entry);
        }
        xccdf_check_content_ref_iterator_free(content_it);
    }

    return files;
}

struct oscap_file_entry_list * xccdf_item_get_systems_and_files(struct xccdf_item * item)
{

    struct xccdf_item_iterator  * child_it;
    struct xccdf_item           * child;
    struct xccdf_check_iterator * check_it;
    struct xccdf_check          * check;
    struct oscap_file_entry_list * files;
    struct oscap_file_entry_list * sub_files;
    struct oscap_file_entry     * file_entry;

    xccdf_type_t itype = xccdf_item_get_type(item);
    files = oscap_file_entry_list_new();

    switch (itype) {
        case XCCDF_RULE:
            check_it = xccdf_rule_get_checks((struct xccdf_rule *) item);
            while(xccdf_check_iterator_has_more(check_it)) {
                check = xccdf_check_iterator_next(check_it);
                sub_files = xccdf_check_get_systems_and_files(check);

                struct oscap_file_entry_iterator *file_it = oscap_file_entry_list_get_files(sub_files);
                while (oscap_file_entry_iterator_has_more(file_it)) {
                    file_entry = (struct oscap_file_entry *) oscap_file_entry_iterator_next(file_it);
                    if (!oscap_list_contains((struct oscap_list *) files, file_entry, (oscap_cmp_func) xccdf_file_entry_cmp_func))
                        oscap_list_add((struct oscap_list *) files, oscap_file_entry_dup(file_entry));
                }
                oscap_file_entry_iterator_free(file_it);
                oscap_file_entry_list_free(sub_files);
            }
            xccdf_check_iterator_free(check_it);
            break;

        case XCCDF_BENCHMARK:
        case XCCDF_GROUP:
            if (itype == XCCDF_GROUP) child_it = xccdf_group_get_content((const struct xccdf_group *)item);
            else child_it = xccdf_benchmark_get_content((const struct xccdf_benchmark *)item);
            while (xccdf_item_iterator_has_more(child_it)) {
                    child = xccdf_item_iterator_next(child_it);
                    sub_files = xccdf_item_get_systems_and_files(child);

                    struct oscap_file_entry_iterator *file_it = oscap_file_entry_list_get_files(sub_files);
                    while (oscap_file_entry_iterator_has_more(file_it)) {
                    	file_entry = (struct oscap_file_entry *) oscap_file_entry_iterator_next(file_it);

                        if (!oscap_list_contains((struct oscap_list *) files, file_entry, (oscap_cmp_func) xccdf_file_entry_cmp_func))
                            oscap_list_add((struct oscap_list *) files, oscap_file_entry_dup(file_entry));
                    }
                    oscap_file_entry_iterator_free(file_it);
                    oscap_file_entry_list_free(sub_files);
            }
            xccdf_item_iterator_free(child_it);
        break;
        
        default: 
            oscap_file_entry_list_free(files);
            files = NULL;
            break;
    } 

    return files;
}

static bool xccdf_cmp_func(const char *s1, const char *s2)
{
    return !oscap_strcmp(s1, s2);
}

static struct oscap_stringlist * xccdf_check_get_files(struct xccdf_check * check)
{
    struct xccdf_check_iterator             * child_it;
    struct xccdf_check                      * child;
    struct xccdf_check_content_ref_iterator * content_it;
    struct xccdf_check_content_ref          * content;
    char                                    * href;
    struct oscap_stringlist                 * names;
    struct oscap_stringlist                 * sub_names;

    names = oscap_stringlist_new();
    if (xccdf_check_get_complex(check)) {
        child_it = xccdf_check_get_children(check);
        while (xccdf_check_iterator_has_more(child_it)) {
            child = xccdf_check_iterator_next(child_it);
            sub_names = xccdf_check_get_files(child);

            struct oscap_string_iterator *name_it = oscap_stringlist_get_strings(sub_names);
            while (oscap_string_iterator_has_more(name_it)) {
                href = (char *) oscap_string_iterator_next(name_it);
                if (!oscap_list_contains((struct oscap_list *) names, (void *) href, (oscap_cmp_func) xccdf_cmp_func))
                    oscap_stringlist_add_string(names, href);
            }
            oscap_string_iterator_free(name_it);
            oscap_stringlist_free(sub_names);
        }
        xccdf_check_iterator_free(child_it);
    } else {
        content_it = xccdf_check_get_content_refs(check);
        while (xccdf_check_content_ref_iterator_has_more(content_it)) {
            content = xccdf_check_content_ref_iterator_next(content_it);
            href = (char *) xccdf_check_content_ref_get_href(content);
            if (!oscap_list_contains((struct oscap_list *) names, href, (oscap_cmp_func) xccdf_cmp_func))
                oscap_stringlist_add_string(names, href);
        }
        xccdf_check_content_ref_iterator_free(content_it);
    }

    return names;
}

struct oscap_stringlist * xccdf_item_get_files(struct xccdf_item * item)
{

    struct xccdf_item_iterator  * child_it;
    struct xccdf_item           * child;
    struct xccdf_check_iterator * check_it;
    struct xccdf_check          * check;
    struct oscap_stringlist     * names;
    struct oscap_stringlist     * sub_names;
    char                        * href;

    xccdf_type_t itype = xccdf_item_get_type(item);
    names = oscap_stringlist_new();

    switch (itype) {
        case XCCDF_RULE:
            check_it = xccdf_rule_get_checks((struct xccdf_rule *) item);
            while(xccdf_check_iterator_has_more(check_it)) {
                check = xccdf_check_iterator_next(check_it);
                sub_names = xccdf_check_get_files(check);

                struct oscap_string_iterator *name_it = oscap_stringlist_get_strings(sub_names);
                while (oscap_string_iterator_has_more(name_it)) {
                    href = (char *) oscap_string_iterator_next(name_it);
                    if (!oscap_list_contains((struct oscap_list *)names, href, (oscap_cmp_func) xccdf_cmp_func))
                        oscap_stringlist_add_string(names, href);
                }
                oscap_string_iterator_free(name_it);
                oscap_stringlist_free(sub_names);
            }
            xccdf_check_iterator_free(check_it);
            break;

        case XCCDF_BENCHMARK:
        case XCCDF_GROUP:
            if (itype == XCCDF_GROUP) child_it = xccdf_group_get_content((const struct xccdf_group *)item);
            else child_it = xccdf_benchmark_get_content((const struct xccdf_benchmark *)item);
            while (xccdf_item_iterator_has_more(child_it)) {
                    child = xccdf_item_iterator_next(child_it);
                    sub_names = xccdf_item_get_files(child);

                    struct oscap_string_iterator *name_it = oscap_stringlist_get_strings(sub_names);
                    while (oscap_string_iterator_has_more(name_it)) {
                        href = (char *) oscap_string_iterator_next(name_it);
                        if (!oscap_list_contains((struct oscap_list *)names, href, (oscap_cmp_func) xccdf_cmp_func))
                            oscap_stringlist_add_string(names, href);
                    }
                    oscap_string_iterator_free(name_it);
                    oscap_stringlist_free(sub_names);
            }
            xccdf_item_iterator_free(child_it);
        break;

        default:
            oscap_stringlist_free(names);
            names = NULL;
            break;
    }

    return names;
}

/***************************************************************************/
/* Public functions.
 */

bool xccdf_policy_model_add_cpe_dict(struct xccdf_policy_model *model, const char * cpe_dict)
{
		__attribute__nonnull__(model);
		__attribute__nonnull__(cpe_dict);

		struct cpe_dict_model* dict = cpe_dict_model_import(cpe_dict);
		return oscap_list_add(model->cpe_dicts, dict);
}

bool xccdf_policy_model_add_cpe_lang_model(struct xccdf_policy_model *model, const char * cpe_lang)
{
		__attribute__nonnull__(model);
		__attribute__nonnull__(cpe_lang);

		struct cpe_lang_model* lang_model = cpe_lang_model_import(cpe_lang);
		return oscap_list_add(model->cpe_lang_models, lang_model);
}
/**
 * Get ID of XCCDF Profile that belongs to XCCDF Policy
 */
const char * xccdf_policy_get_id(struct xccdf_policy * policy)
{
    if (policy->profile != NULL)
        return xccdf_profile_get_id(xccdf_policy_get_profile(policy));
    else return NULL;
}

/**
 * Funtion to register callback for particular checking system. System is used for evaluating content
 * of rules.
 */
bool xccdf_policy_model_register_engine_callback(struct xccdf_policy_model * model, char * sys, void * func, void * usr)
{
	return xccdf_policy_model_register_engine_and_query_callback(model, sys, func, usr, NULL);
}

bool xccdf_policy_model_register_engine_and_query_callback(struct xccdf_policy_model *model, char *sys, void *func, void *usr, xccdf_policy_engine_query_fn query_fn)
{
        __attribute__nonnull__(model);
        callback * cb = oscap_alloc(sizeof(callback));
        if (cb == NULL) return false;

        cb->system   = sys;
        cb->callback = func;
        cb->usr      = usr;
	cb->query_fn = query_fn;

        return oscap_list_add(model->callbacks, cb);
}

bool xccdf_policy_model_register_start_callback(struct xccdf_policy_model * model, policy_reporter_start func, void * usr)
{

        __attribute__nonnull__(model);
        callback_out * cb = oscap_alloc(sizeof(callback_out));
        if (cb == NULL) return false;

        cb->system   = "urn:xccdf:system:callback:start";
        cb->callback = (void*)func;
        cb->usr      = usr;

        return oscap_list_add(model->callbacks, (callback *) cb);
}

bool xccdf_policy_model_register_output_callback(struct xccdf_policy_model * model, policy_reporter_output func, void * usr)
{

        __attribute__nonnull__(model);
        callback_out * cb = oscap_alloc(sizeof(callback_out));
        if (cb == NULL) return false;

        cb->system   = "urn:xccdf:system:callback:output";
        cb->callback = (void*)func;
        cb->usr      = usr;

        return oscap_list_add(model->callbacks, (callback *) cb);
}

struct xccdf_result * xccdf_policy_get_result_by_id(struct xccdf_policy * policy, const char * id) {

    struct xccdf_result_iterator    * result_it;
    struct xccdf_result             * result;

    result_it = xccdf_policy_get_results(policy);
    while (xccdf_result_iterator_has_more(result_it)){
        result = xccdf_result_iterator_next(result_it);
        if (!strcmp(xccdf_result_get_id(result), id) ){
            xccdf_result_iterator_free(result_it);
            return result;
        }
    }
    xccdf_result_iterator_free(result_it);
    return NULL;
}

/***************************************************************************/
/* Constructors of XCCDF POLICY structures xccdf_policy_*<structure>*_new()
 * More info in representive header file.
 * returns the type of <structure>
 */

/**
 * New XCCDF Policy model. Create new structure and fill the policies list with 
 * policy entries that are inherited from XCCDF benchmark Profile elements. For each 
 * profile element call xccdf_policy_new function.
 */
struct xccdf_policy_model * xccdf_policy_model_new(struct xccdf_benchmark * benchmark) {

	__attribute__nonnull__(benchmark);

	struct xccdf_policy_model       * model;
        struct xccdf_profile_iterator   * profile_it;
        struct xccdf_profile            * profile;
        struct xccdf_policy             * policy;

	model = oscap_alloc(sizeof(struct xccdf_policy_model));
	if (model == NULL)
		return NULL;
	memset(model, 0, sizeof(struct xccdf_policy_model));

	model->benchmark = benchmark;
	model->policies  = oscap_list_new();
        model->callbacks = oscap_list_new();
	model->cpe_dicts = oscap_list_new();
	model->cpe_lang_models = oscap_list_new();
	model->cpe_oval_sessions = oscap_htable_new();

        /* Resolve document */
        xccdf_benchmark_resolve(benchmark);

        /* Create policy without profile */
        profile = xccdf_profile_new();
        xccdf_profile_set_id(profile, NULL);
        struct oscap_text * title = oscap_text_new();
        oscap_text_set_text(title, "No profile (default benchmark)");
        oscap_text_set_lang(title, "en");
        xccdf_profile_add_title(profile, title);
        policy = xccdf_policy_new(model, profile); 
        if (policy != NULL) oscap_list_add(model->policies, policy);

        /* Create policies from benchmark model */
        profile_it = xccdf_benchmark_get_profiles(benchmark);
        /* Iterate through profiles and create policies */
        while (xccdf_profile_iterator_has_more(profile_it)) {

            profile = xccdf_profile_iterator_next(profile_it);
            policy = xccdf_policy_new(model, profile);

            /* Should we set the error code and return NULL here ? */
            if (policy != NULL) oscap_list_add(model->policies, policy);
            else {
                xccdf_profile_iterator_free(profile_it);
                xccdf_policy_model_free(model);
                return NULL;
            }
        }
        xccdf_profile_iterator_free(profile_it);

	return model;
}

/**
 * Constructor for structure XCCDF Policy. Create the structure and resolve all rules
 * from benchmark that are not present in selectors. This step is necessary because of 
 * default values of groups / rules that can be added to Policy whether or not they have
 * their selector. Last step is filling the list of value bindings (see more in 
 * xccdf_value_binding_proccess).
 */
struct xccdf_policy * xccdf_policy_new(struct xccdf_policy_model * model, struct xccdf_profile * profile) {

	struct xccdf_policy             * policy;
        struct xccdf_benchmark          * benchmark;
        struct xccdf_item_iterator      * item_it;
        struct xccdf_item               * item;
        struct xccdf_select             * sel;
        struct xccdf_select_iterator    * sel_it = NULL;

	policy = oscap_alloc(sizeof(struct xccdf_policy));
	if (policy == NULL)
		return NULL;
	memset(policy, 0, sizeof(struct xccdf_policy));

	policy->profile = profile;
	policy->selects = oscap_list_new();
	policy->values  = oscap_list_new();
        policy->results = oscap_list_new();

        policy->ht_selects = NULL;
        policy->model = model;

        /* Create selects from benchmark model */
        if (profile != NULL)
            sel_it = xccdf_profile_get_selects(profile);
        /* Iterate through selects in profile */
        while (xccdf_select_iterator_has_more(sel_it)) {

            sel = xccdf_select_iterator_next(sel_it);
            /* Should we set the error code and return NULL here ? */
            if (sel != NULL) oscap_list_add(policy->selects, xccdf_select_clone(sel));
        }
        xccdf_select_iterator_free(sel_it);

        /* Iterate through items in benchmark and resolve rules */
        benchmark = xccdf_policy_model_get_benchmark(model);
        item_it = xccdf_benchmark_get_content(benchmark);
        while (xccdf_item_iterator_has_more(item_it)) {
            item = xccdf_item_iterator_next(item_it);
            xccdf_policy_resolve_item(policy, item, true);
        }
        xccdf_item_iterator_free(item_it);

        /* Create hash table for selects
         * s_count -> number of selects to create optimal size of
         * hash table
         */
        sel_it = xccdf_policy_get_selects(policy);
        const int HTABLE_MIN_SIZE = 256;
        int s_count = oscap_iterator_get_itemcount((struct oscap_iterator *) sel_it);
        if (s_count < HTABLE_MIN_SIZE)
            s_count = HTABLE_MIN_SIZE;
        policy->ht_selects = oscap_htable_new1((oscap_compare_func) strcmp, s_count);
        if (policy->ht_selects == NULL) {
	    oscap_dlprintf(DBG_E, "Can't make hash table for policy selects\n");
            xccdf_select_iterator_free(sel_it);
            return policy;
        }
        while (xccdf_select_iterator_has_more(sel_it)) {
            sel = xccdf_select_iterator_next(sel_it);
            oscap_htable_add(policy->ht_selects, xccdf_select_get_item(sel), sel);
        }
        xccdf_select_iterator_free(sel_it);

	return policy;
}

/**
 * Constructor for structure XCCDF Policy Value bindings
 */
struct xccdf_value_binding * xccdf_value_binding_new() {

        struct xccdf_value_binding          * binding;

        /* Initialization */
	binding = oscap_alloc(sizeof(struct xccdf_value_binding));
	if (binding == NULL)
		return NULL;
	memset(binding, 0, sizeof(struct xccdf_value_binding));

        binding->name       = NULL;
        binding->type       = 0;
        binding->value      = NULL;
        binding->setvalue   = NULL;
        binding->operator   = XCCDF_OPERATOR_EQUALS;

        return binding;
}

/***************************************************************************/

/**
 * If policy has the select specified by item_id return the select, NULL otherwise
 */
struct xccdf_select * xccdf_policy_get_select_by_id(struct xccdf_policy * policy, const char *item_id)
{
        __attribute__nonnull__(policy);
        
        struct xccdf_select_iterator    * sel_it;
        struct xccdf_select             * sel = NULL;

        if (policy->ht_selects != NULL) {
            /* We have hash table -> faster
             */
            sel = oscap_htable_get(policy->ht_selects, item_id);
        }
        if (sel == NULL) {
            /* We don't have hash table :( -> do it old way
             */
            sel_it = xccdf_policy_get_selects(policy);
            while (xccdf_select_iterator_has_more(sel_it)) {
                    sel = xccdf_select_iterator_next(sel_it);
                    if (!strcmp(xccdf_select_get_item(sel), item_id)) {
                        xccdf_select_iterator_free(sel_it);
                        return sel;
                    }
            }
            xccdf_select_iterator_free(sel_it);
        } else return sel;

        return NULL;
}


struct xccdf_select_iterator * xccdf_policy_get_selected_rules(struct xccdf_policy * policy) {

    return (struct xccdf_select_iterator *) oscap_iterator_new_filter( policy->selects, 
                                                                       (oscap_filter_func) xccdf_policy_filter_selected, 
                                                                       policy);
}

/**
 * Make the rule from benchmark selected in Policy
 */
bool xccdf_policy_set_selected(struct xccdf_policy * policy, char * idref) {
    bool ret;
    struct oscap_iterator *sel_it = 
        oscap_iterator_new_filter( policy->selects, (oscap_filter_func) xccdf_policy_filter_select, idref);
    if (oscap_iterator_get_itemcount(sel_it) > 0) {
        /* There is rule already, skip */
        ret = 0;
    }
    else {
        /* There is no such rule, add */
        struct xccdf_select * sel = NULL;
        //TODO: sel = xccdf_select_new <-- missing implementation
        oscap_list_add(policy->selects, sel);
        ret = 1;

    }
    oscap_iterator_free(sel_it);
    return ret;
}

/**
 * Get Policy from Policy model by it's id.
 */
struct xccdf_policy * xccdf_policy_model_get_policy_by_id(struct xccdf_policy_model * policy_model, const char * id)
{
    struct xccdf_policy_iterator * policy_it;
    struct xccdf_policy          * policy;

    policy_it = xccdf_policy_model_get_policies(policy_model);
    if (id == NULL) {
        while (xccdf_policy_iterator_has_more(policy_it)) {
            policy = xccdf_policy_iterator_next(policy_it);
            if (xccdf_policy_get_id(policy) == NULL) {
                xccdf_policy_iterator_free(policy_it);
                return policy;
            }
        }
        xccdf_policy_iterator_free(policy_it);
        return NULL;
    }

    while (xccdf_policy_iterator_has_more(policy_it)) {
        policy = xccdf_policy_iterator_next(policy_it);
        if (xccdf_policy_get_id(policy) == NULL) continue;
        if (!strcmp(xccdf_policy_get_id(policy), id)) {
            xccdf_policy_iterator_free(policy_it);
            return policy;
        }
    }
    xccdf_policy_iterator_free(policy_it);

    return NULL;
}

/**
 * Resolve benchmark - apply all refine_rules to the benchmark items.
 * Beware ! Benchmark properties will be changed ! For discarding changes you have to load
 * benchmark from XML file again.
 */
bool xccdf_policy_resolve(struct xccdf_policy * policy)
{

    struct xccdf_refine_rule_iterator   * r_rule_it;
    struct xccdf_refine_rule            * r_rule;
    struct xccdf_item                   * item;

    struct xccdf_policy_model           * policy_model  = xccdf_policy_get_model(policy);
    struct xccdf_benchmark              * benchmark     = xccdf_policy_model_get_benchmark(policy_model);
    struct xccdf_profile                * profile       = xccdf_policy_get_profile(policy);

    /* Proccess refine rules; Changing Rules and Groups */
    r_rule_it = xccdf_profile_get_refine_rules(profile);
    while (xccdf_refine_rule_iterator_has_more(r_rule_it)) {
        r_rule = xccdf_refine_rule_iterator_next(r_rule_it);
        item = xccdf_benchmark_get_item(benchmark, xccdf_refine_rule_get_item(r_rule));
        if (item != NULL) {
            /* Proccess refine rule appliement */
            /* In r_rule we have refine rule that match  - no more then one !*/
            if (xccdf_item_get_type(item) == XCCDF_GROUP) { 
                /* Perform check of weight attribute  - ignore other attributes */
                if (xccdf_refine_rule_weight_defined(r_rule)) {
                        oscap_seterr(OSCAP_EFAMILY_XCCDF, "'Weight' attribute not specified, only 'weight' attribute applies to groups items");
                        xccdf_refine_rule_iterator_free(r_rule_it);
                        return false;            
                }
                else {
                    /* Apply the rule changes */
                    xccdf_group_set_weight((struct xccdf_group *) item, xccdf_refine_rule_get_weight(r_rule) );
                }
                
            } else if (xccdf_item_get_type(item) == XCCDF_RULE) {
                /* Perform all changes in rule */
                if (!isnan(xccdf_refine_rule_get_role(r_rule)))
                    xccdf_rule_set_role((struct xccdf_rule *) item, xccdf_refine_rule_get_role(r_rule));
                if (!isnan(xccdf_refine_rule_get_severity(r_rule)))
                    xccdf_rule_set_severity((struct xccdf_rule *) item, xccdf_refine_rule_get_severity(r_rule));

            } else {}/* TODO oscap_err ? */;

        } else {
            oscap_seterr(OSCAP_EFAMILY_XCCDF, "Refine rule item points to nonexisting XCCDF item");
            xccdf_refine_rule_iterator_free(r_rule_it);
            return false;            
        }
    }
    xccdf_refine_rule_iterator_free(r_rule_it);

    return true;
}

/**
 * Evaluate XCCDF Policy
 * Iterate through Benchmark items and evalute one by one by calling 
 * callback for checking system that is defined by particular rules
 * Callbacks for checking systems have to be defined before calling this function, otherwise 
 * rules would not be evaluated and process ends with error.
 */
struct xccdf_result * xccdf_policy_evaluate(struct xccdf_policy * policy)
{
    struct xccdf_select_iterator    * sel_it;
    struct xccdf_select             * sel;
    struct xccdf_item               * item;
    struct xccdf_benchmark          * benchmark;
    int                               ret       = -1;
    const char			    * doc_version = NULL;

    __attribute__nonnull__(policy);

    /* Add result to policy */
    struct xccdf_result * result = xccdf_result_new();
    xccdf_result_set_start_time(result, time(NULL));

    /** Set ID of TestResult */
    const char * id = NULL;
    if ((xccdf_policy_get_profile(policy) != NULL) && (xccdf_profile_get_id(xccdf_policy_get_profile(policy)) != NULL))
        id = oscap_strdup(xccdf_profile_get_id(xccdf_policy_get_profile(policy)));
    else
        id = oscap_strdup("default-profile");

    /* Get all constant information */
    benchmark = xccdf_policy_model_get_benchmark(xccdf_policy_get_model(policy));
    const struct xccdf_version_info* version_info = xccdf_benchmark_get_schema_version(benchmark);
    doc_version = xccdf_version_info_get_version(version_info);

    if (strverscmp("1.2", doc_version) >= 0)
    {
        // we have to enforce a certain type of ids for XCCDF 1.2+

        char rid[32+strlen(id)];
        sprintf(rid, "xccdf_org.open-scap_testresult_%s", id);
        xccdf_result_set_id(result, rid);
    }
    else
    {
    	// previous behaviour for backwards compatibility

        char rid[11+strlen(id)];
        sprintf(rid, "OSCAP-Test-%s", id);
        xccdf_result_set_id(result, rid);
    }

    oscap_free(id);
    /** */


    sel_it = xccdf_policy_get_selects(policy);
    while (xccdf_select_iterator_has_more(sel_it)) {
        sel = xccdf_select_iterator_next(sel_it);

        /* Get the refid string and find xccdf_item in benchmark */
        /* TODO: we need to check if every requirement is met - some of required Item has to be sleected too */

        item = xccdf_benchmark_get_item(benchmark, xccdf_select_get_item(sel));
        if (item == NULL) {
            oscap_seterr(OSCAP_EFAMILY_XCCDF, "Selector ID(%s) does not exist in Benchmark.", xccdf_select_get_item(sel));
            continue; /* TODO: Should we just skip that selector ? XCCDF is not valid here !! */
        }

        if (xccdf_item_get_type(item) == XCCDF_GROUP) continue;
        ret = xccdf_policy_item_evaluate(policy, item, result);
        if (ret == -1) {
            xccdf_select_iterator_free(sel_it);
            xccdf_result_free(result);
            return NULL;
        }
        if (ret != 0) break;
    }
    xccdf_select_iterator_free(sel_it);
    xccdf_policy_add_result(policy, result);
    xccdf_result_set_end_time(result, time(NULL));

    return result;
}

struct xccdf_score * xccdf_policy_get_score(struct xccdf_policy * policy, struct xccdf_result * test_result, const char * scsystem)
{
    struct xccdf_score * score = NULL;
    struct xccdf_benchmark * benchmark = xccdf_policy_model_get_benchmark(xccdf_policy_get_model(policy));

    score = xccdf_score_new();
    xccdf_score_set_system(score, scsystem);
    /* Default XCCDF score system */
    if (!strcmp(scsystem, "urn:xccdf:scoring:default")) {
        struct xccdf_default_score * item_score = xccdf_item_get_default_score((struct xccdf_item *) benchmark, test_result);
        xccdf_score_set_score(score, item_score->score);
        oscap_free(item_score);
    }
    else if (!strcmp(scsystem, "urn:xccdf:scoring:flat")) {
        struct xccdf_flat_score * item_score = xccdf_item_get_flat_score((struct xccdf_item *) benchmark, test_result, false);
        xccdf_score_set_maximum(score, item_score->weight);
        xccdf_score_set_score(score, item_score->score);
        oscap_free(item_score);
    }
    else if (!strcmp(scsystem, "urn:xccdf:scoring:flat-unweighted")) {
        struct xccdf_flat_score * item_score = xccdf_item_get_flat_score((struct xccdf_item *) benchmark, test_result, true);
        xccdf_score_set_maximum(score, item_score->weight);
        xccdf_score_set_score(score, item_score->score);
        oscap_free(item_score);
    }
    else if (!strcmp(scsystem, "urn:xccdf:scoring:absolute")) {
        int absolute;
        struct xccdf_flat_score * item_score = xccdf_item_get_flat_score((struct xccdf_item *) benchmark, test_result, false);
        xccdf_score_set_maximum(score, item_score->weight);
        absolute = (item_score->score == item_score->weight);
        xccdf_score_set_score(score, absolute);
        oscap_free(item_score);
    } else {
        xccdf_score_free(score);
        oscap_dlprintf(DBG_E, "Scoring system \"%s\" is not supported.\n", scsystem);
        return NULL;
    }

    return score;
}

static struct xccdf_refine_rule * xccdf_policy_get_refine_rules_by_rule(struct xccdf_policy * policy, struct xccdf_item * item)
{
    struct xccdf_refine_rule * r_rule = NULL;
    struct xccdf_profile * profile = xccdf_policy_get_profile(policy);
    if (profile == NULL) return NULL;

    /* Get refine-rule for this item */
    struct xccdf_refine_rule_iterator * r_rule_it = xccdf_profile_get_refine_rules(profile);
    while (xccdf_refine_rule_iterator_has_more(r_rule_it)) {
        r_rule = xccdf_refine_rule_iterator_next(r_rule_it);
        if (!strcmp(xccdf_refine_rule_get_item(r_rule), xccdf_rule_get_id((struct xccdf_rule *) item)))
            break;
        else r_rule = NULL;
    }
    xccdf_refine_rule_iterator_free(r_rule_it);

    return r_rule;
}

static const char * xccdf_policy_get_value_of_item(struct xccdf_policy * policy, struct xccdf_item * item)
{
    struct xccdf_refine_value * r_value = NULL;
    struct xccdf_setvalue * s_value = NULL;
    const char * selector = NULL;
    struct xccdf_profile * profile = xccdf_policy_get_profile(policy);
    if (profile == NULL) return NULL;

    /* Get set_value for this item */
    struct xccdf_setvalue_iterator * s_value_it = xccdf_profile_get_setvalues(profile);
    while (xccdf_setvalue_iterator_has_more(s_value_it)) {
        s_value = xccdf_setvalue_iterator_next(s_value_it);
        if (!strcmp(xccdf_setvalue_get_item(s_value), xccdf_value_get_id((struct xccdf_value *) item)))
            break;
        else s_value = NULL;
    }
    xccdf_setvalue_iterator_free(s_value_it);
    if (s_value != NULL) return xccdf_setvalue_get_value(s_value);

    /* We don't have set-value in profile, look for refine-value */
    struct xccdf_refine_value_iterator * r_value_it = xccdf_profile_get_refine_values(profile);
    while (xccdf_refine_value_iterator_has_more(r_value_it)) {
        r_value = xccdf_refine_value_iterator_next(r_value_it);
        if (!strcmp(xccdf_refine_value_get_item(r_value), xccdf_value_get_id((struct xccdf_value *) item)))
            break;
        else r_value = NULL;
    }
    xccdf_refine_value_iterator_free(r_value_it);
    if (r_value != NULL) {
        selector = xccdf_refine_value_get_selector(r_value);
        struct xccdf_value_instance * instance = xccdf_value_get_instance_by_selector((struct xccdf_value *) item, selector);
        return xccdf_value_instance_get_value(instance);
    }

    return NULL;
}

static int xccdf_policy_get_refine_value_oper(struct xccdf_policy * policy, struct xccdf_item * item)
{
    struct xccdf_refine_value * r_value = NULL;
    struct xccdf_profile * profile = xccdf_policy_get_profile(policy);
    if (profile == NULL) return -1;

    /* We don't have set-value in profile, look for refine-value */
    struct xccdf_refine_value_iterator * r_value_it = xccdf_profile_get_refine_values(profile);
    while (xccdf_refine_value_iterator_has_more(r_value_it)) {
        r_value = xccdf_refine_value_iterator_next(r_value_it);
        if (!strcmp(xccdf_refine_value_get_item(r_value), xccdf_value_get_id((struct xccdf_value *) item)))
            break;
        else r_value = NULL;
    }
    xccdf_refine_value_iterator_free(r_value_it);
    if (r_value != NULL) {
        return xccdf_refine_value_get_oper(r_value);
    }
    return -1;
}

struct xccdf_item * xccdf_policy_tailor_item(struct xccdf_policy * policy, struct xccdf_item * item)
{
    struct xccdf_item * new_item = NULL;

    xccdf_type_t type = xccdf_item_get_type(item);
    switch (type) {
        case XCCDF_RULE: {
            struct xccdf_refine_rule * r_rule = xccdf_policy_get_refine_rules_by_rule(policy, item);
            if (r_rule == NULL) return item;

            new_item = (struct xccdf_item *) xccdf_rule_clone((struct xccdf_rule *) item);
            if (!isnan(xccdf_refine_rule_get_role(r_rule)))
                xccdf_rule_set_role((struct xccdf_rule *) new_item, xccdf_refine_rule_get_role(r_rule));
            if (!isnan(xccdf_refine_rule_get_severity(r_rule)))
                xccdf_rule_set_severity((struct xccdf_rule *) new_item, xccdf_refine_rule_get_severity(r_rule));
            if (xccdf_refine_rule_weight_defined(r_rule))
                xccdf_rule_set_weight((struct xccdf_rule *) new_item, xccdf_refine_rule_get_weight(r_rule));
            break;
        }
        case XCCDF_GROUP: {
            struct xccdf_refine_rule * r_rule = xccdf_policy_get_refine_rules_by_rule(policy, item);
            if (r_rule == NULL) return item;

            new_item = (struct xccdf_item *) xccdf_group_clone((struct xccdf_group *) item);
            if (xccdf_refine_rule_weight_defined(r_rule))
                xccdf_group_set_weight((struct xccdf_group *) new_item, xccdf_refine_rule_get_weight(r_rule));
            else {
                xccdf_group_free(new_item);
                return item;
            }
            break;
        }
        case XCCDF_VALUE: {
            const char * value = xccdf_policy_get_value_of_item(policy, item);
            if (value == NULL) 
                return NULL;

            const char * selector = NULL;
            struct xccdf_value_instance * instance = NULL;
            new_item = (struct xccdf_item *) xccdf_value_clone((struct xccdf_value *) item);

            struct xccdf_value_instance_iterator * instance_it = xccdf_value_get_instances((struct xccdf_value *) new_item);
            while (xccdf_value_instance_iterator_has_more(instance_it)) {
                instance = xccdf_value_instance_iterator_next(instance_it);
                if (!oscap_strcmp(xccdf_value_instance_get_value(instance), value))
                    selector = xccdf_value_instance_get_selector(instance);
            }
            xccdf_value_instance_iterator_free(instance_it);
            instance_it = xccdf_value_get_instances((struct xccdf_value *) new_item);
            while (xccdf_value_instance_iterator_has_more(instance_it)) {
                instance = xccdf_value_instance_iterator_next(instance_it);
                if (oscap_strcmp(xccdf_value_instance_get_selector(instance), selector))
                    xccdf_value_instance_iterator_remove(instance_it);
            }
            xccdf_value_instance_iterator_free(instance_it);
            if (selector == NULL) {
                instance = xccdf_value_get_instance_by_selector((struct xccdf_value *) new_item, NULL);
                xccdf_value_instance_set_defval_string(instance, value);
            }

            int oper = xccdf_policy_get_refine_value_oper(policy, item);
            if (oper == -1) break;
            xccdf_value_set_oper((struct xccdf_value *) item, (xccdf_operator_t) oper);
            break;
        }
        default:
            return NULL;
    }
    return new_item;
}

struct oscap_file_entry_list * xccdf_policy_model_get_systems_and_files(struct xccdf_policy_model * policy_model)
{
    return xccdf_item_get_systems_and_files((struct xccdf_item *) xccdf_policy_model_get_benchmark(policy_model));
}

struct oscap_stringlist * xccdf_policy_model_get_files(struct xccdf_policy_model * policy_model)
{
    return xccdf_item_get_files((struct xccdf_item *) xccdf_policy_model_get_benchmark(policy_model));
}

static void _xccdf_policy_destroy_cpe_oval_session(void* ptr)
{
	struct oval_agent_session* session = (struct oval_agent_session*)ptr;
	struct oval_definition_model* model = oval_agent_get_definition_model(session);
	oval_agent_destroy_session(session);
	oval_definition_model_free(model);
}

void xccdf_policy_model_free(struct xccdf_policy_model * model) {

	oscap_list_free(model->policies, (oscap_destruct_func) xccdf_policy_free);
	oscap_list_free(model->callbacks, (oscap_destruct_func) oscap_free);
        xccdf_benchmark_free(model->benchmark);
	oscap_list_free(model->cpe_dicts, (oscap_destruct_func) cpe_dict_model_free);
	oscap_list_free(model->cpe_lang_models, (oscap_destruct_func) cpe_lang_model_free);
	oscap_htable_free(model->cpe_oval_sessions, (oscap_destruct_func) _xccdf_policy_destroy_cpe_oval_session);
        oscap_free(model);
}

void xccdf_policy_free(struct xccdf_policy * policy) {

        /* If ID of policy's profile is NULL then this
         * profile is created by Policy layer and need
         * to be freed
         */
        if (xccdf_profile_get_id(policy->profile) == NULL)
            xccdf_profile_free((struct xccdf_item *) policy->profile);

	oscap_list_free(policy->selects, (oscap_destruct_func) xccdf_select_free);
	oscap_list_free(policy->values, (oscap_destruct_func) xccdf_value_binding_free);
	oscap_list_free(policy->results, (oscap_destruct_func) xccdf_result_free);
        oscap_htable_free(policy->ht_selects, (oscap_destruct_func) NULL);
        oscap_free(policy);
}

void xccdf_value_binding_free(struct xccdf_value_binding * binding) {

        oscap_free(binding->name);
        oscap_free(binding->value);
        oscap_free(binding->setvalue);
        oscap_free(binding);
}

// textual substitiution implementation

static char* xccdf_subst_callback(xccdf_subst_type_t type, const char *id, void *arg) {

    if (id == NULL || arg == NULL)
        return NULL;

    struct xccdf_policy *policy = arg;
    struct xccdf_policy_model *model = xccdf_policy_get_model(policy);
    if (model == NULL) return NULL;
    struct xccdf_benchmark *bench = xccdf_policy_model_get_benchmark(model);
    if (bench == NULL) return NULL;

    switch (type) {
        case XCCDF_SUBST_SUB: {

            // try substitute a value form <cdf:plain_text> element
            const char *subst = xccdf_benchmark_get_plain_text(bench, id);
            if (subst) return oscap_strdup(subst);

            // try substitute a Value value
            struct xccdf_item *value = xccdf_benchmark_get_item(bench, id);
            if (value && xccdf_item_get_type(value) == XCCDF_VALUE) {
                char *ret = oscap_strdup("");
                struct xccdf_value *value_tailored = xccdf_item_to_value(xccdf_policy_tailor_item(policy, value));
                struct xccdf_value_instance_iterator *it = xccdf_value_get_instances(value_tailored);
                if (xccdf_value_instance_iterator_has_more(it)) {
                    struct xccdf_value_instance *inst = xccdf_value_instance_iterator_next(it);
                    oscap_free(ret);
                    ret = oscap_strdup(xccdf_value_instance_get_value(inst));
                }
                xccdf_value_instance_iterator_free(it);
                xccdf_item_free(xccdf_value_to_item(value_tailored));
                return ret;
            }
            break;
        }
        default: return NULL; // TODO implement other substitution types
    }

    return NULL;
}

char* xccdf_policy_substitute(const char *text, struct xccdf_policy *policy) {
    return oscap_text_xccdf_substitute(text, xccdf_subst_callback, policy);
}

