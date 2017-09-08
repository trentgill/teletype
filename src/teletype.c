#include <stdint.h>  // types
#include <stdio.h>   // printf
#include <string.h>
#include <unistd.h>  // ssize_t

#include "helpers.h"
#include "ops/op.h"
#include "scanner.h"
#include "table.h"
#include "teletype.h"
#include "teletype_io.h"
#include "util.h"


/////////////////////////////////////////////////////////////////
// DELAY ////////////////////////////////////////////////////////

void clear_delays(scene_state_t *ss) {
    for (int16_t i = 0; i < TR_COUNT; i++) { ss->tr_pulse_timer[i] = 0; }

    for (int16_t i = 0; i < DELAY_SIZE; i++) { ss->delay.time[i] = 0; }

    ss->delay.count = 0;
    ss->stack_op.top = 0;

    tele_has_delays(false);
    tele_has_stack(false);
}


/////////////////////////////////////////////////////////////////
// PARSE ////////////////////////////////////////////////////////

error_t parse(const char *cmd, tele_command_t *out,
              char error_msg[TELE_ERROR_MSG_LENGTH]) {
    // call the Ragel generated scanner function
    return scanner(cmd, out, error_msg);
}

/////////////////////////////////////////////////////////////////
// VALIDATE /////////////////////////////////////////////////////

error_t validate(const tele_command_t *c,
                 char error_msg[TELE_ERROR_MSG_LENGTH]) {
    error_msg[0] = 0;
    int16_t stack_depth = 0;
    uint8_t idx = c->length;
    int8_t sep_count = 0;

    while (idx--) {  // process words right to left
        tele_word_t word_type = c->data[idx].tag;
        int16_t word_value = c->data[idx].value;
        // A first_cmd is either at the beginning of the command or immediately
        // after the PRE_SEP or COMMAND_SEP
        bool first_cmd = idx == 0 || c->data[idx - 1].tag == PRE_SEP ||
                         c->data[idx - 1].tag == SUB_SEP;

        if (word_type == NUMBER) { stack_depth++; }
        else if (word_type == OP) {
            const tele_op_t *op = tele_ops[word_value];

            // if we're not a first_cmd we need to return something
            if (!first_cmd && !op->returns) {
                strcpy(error_msg, op->name);
                return E_NOT_LEFT;
            }

            stack_depth -= op->params;

            if (stack_depth < 0) {
                strcpy(error_msg, op->name);
                return E_NEED_PARAMS;
            }

            stack_depth += op->returns ? 1 : 0;

            // if we are in the first_cmd position and there is a set fn
            // decrease the stack depth
            // TODO this is technically wrong. the only reason we get away with
            // it is that it's idx == 0, and the while loop is about to end.
            if (first_cmd && op->set != NULL) stack_depth--;
        }
        else if (word_type == MOD) {
            error_t mod_error = E_OK;

            if (idx != 0)
                mod_error = E_NO_MOD_HERE;
            else if (c->separator == -1)
                mod_error = E_NEED_PRE_SEP;
            else if (stack_depth < tele_mods[word_value]->params)
                mod_error = E_NEED_PARAMS;
            else if (stack_depth > tele_mods[word_value]->params)
                mod_error = E_EXTRA_PARAMS;

            if (mod_error != E_OK) {
                strcpy(error_msg, tele_mods[word_value]->name);
                return mod_error;
            }

            stack_depth = 0;
        }
        else if (word_type == PRE_SEP) {
            sep_count++;
            if (sep_count > 1) return E_MANY_PRE_SEP;

            if (idx == 0) return E_PLACE_PRE_SEP;

            if (c->data[0].tag != MOD) return E_PLACE_PRE_SEP;

            if (stack_depth > 1) return E_EXTRA_PARAMS;

            // reset the stack depth
            stack_depth = 0;
        }
        else if (word_type == SUB_SEP) {
            if (sep_count > 0) return E_NO_SUB_SEP_IN_PRE;

            if (stack_depth > 1) return E_EXTRA_PARAMS;

            // reset the stack depth
            stack_depth = 0;
        }
    }

    if (stack_depth > 1)
        return E_EXTRA_PARAMS;
    else
        return E_OK;
}

/////////////////////////////////////////////////////////////////
// RUN //////////////////////////////////////////////////////////

process_result_t run_script(scene_state_t *ss, size_t script_no) {
    exec_state_t es;
    es_init(&es);
    return run_script_with_exec_state(ss, &es, script_no);
}

process_result_t run_script_with_exec_state(scene_state_t *ss, exec_state_t *es,
                                            size_t script_no) {
    process_result_t result = {.has_value = false, .value = 0 };

    // increase the execution depth on each call (e.g. from SCRIPT)
    es->exec_depth++;
    // only allow the depth to reach 8
    // (if we want to allow this number to be any bigger we really should
    // convert this recursive call to use some sort of trampoline!)
    if (es->exec_depth > 8) { return result; }

    for (size_t i = 0; i < ss_get_script_len(ss, script_no); i++) {
        result =
            process_command(ss, es, ss_get_script_command(ss, script_no, i));
    }

    // decrease the depth once the commands have been run
    es->exec_depth--;

    return result;
}

process_result_t run_command(scene_state_t *ss, const tele_command_t *cmd) {
    exec_state_t es;
    es_init(&es);
    return process_command(ss, &es, cmd);
}


/////////////////////////////////////////////////////////////////
// PROCESS //////////////////////////////////////////////////////

// run a single command inside a given exec_state
process_result_t process_command(scene_state_t *ss, exec_state_t *es,
                                 const tele_command_t *c) {
    command_state_t cs;
    cs_init(&cs);  // initialise this here as well as inside the loop, in case
                   // the command has 0 length

    // 1. Do we have a PRE seperator?
    // ------------------------------
    // if we do then only process the PRE part, the MOD will determine if the
    // POST should be run and take care of running it
    ssize_t start_idx = 0;
    ssize_t end_idx = c->separator == -1 ? c->length : c->separator;

    // 2. Determine the location of all the SUB commands
    // -------------------------------------------------
    // an array of structs to hold the start and end of each sub command
    struct sub_idx {
        ssize_t start;
        ssize_t end;
    } subs[COMMAND_MAX_LENGTH];

    ssize_t sub_len = 0;
    ssize_t sub_start = 0;

    // iterate through c->data to find all the SUB_SEPs and add to the array
    for (ssize_t idx = start_idx; idx < end_idx; idx++) {
        tele_word_t word_type = c->data[idx].tag;
        if (word_type == SUB_SEP && idx > sub_start) {
            subs[sub_len].start = sub_start;
            subs[sub_len].end = idx - 1;
            sub_len++;
            sub_start = idx + 1;
        }
    }

    // the last sub command won't have been added, manually add it here
    if (end_idx > sub_start) {
        subs[sub_len].start = sub_start;
        subs[sub_len].end = end_idx - 1;
        sub_len++;
    }

    // 3. Loop through each sub command and execute it
    // -----------------------------------------------
    // iterate through sub commands from left to right
    for (ssize_t sub_idx = 0; sub_idx < sub_len; sub_idx++) {
        const ssize_t sub_start = subs[sub_idx].start;
        const ssize_t sub_end = subs[sub_idx].end;

        // initialise the command state for each sub, otherwise a value left on
        // the stack for the previous sub, can cause the set fn to trigger when
        // it shouldn't
        cs_init(&cs);

        // as we are using a stack based language, we must process commands from
        // right to left
        for (ssize_t idx = sub_end; idx >= sub_start; idx--) {
            const tele_word_t word_type = c->data[idx].tag;
            const int16_t word_value = c->data[idx].value;

            if (word_type == NUMBER) { cs_push(&cs, word_value); }
            else if (word_type == OP) {
                const tele_op_t *op = tele_ops[word_value];

                // if we're in the first command position, and there is a set fn
                // pointer and we have enough params, then run set, else run get
                if (idx == sub_start && op->set != NULL &&
                    cs_stack_size(&cs) >= op->params + 1)
                    op->set(op->data, ss, es, &cs);
                else
                    op->get(op->data, ss, es, &cs);
            }
            else if (word_type == MOD) {
                tele_command_t post_command;
                copy_post_command(&post_command, c);
                tele_mods[word_value]->func(ss, es, &cs, &post_command);
            }
        }
    }

    // 4. Return
    // ---------
    // sometimes we have single value left of the stack, if so return it
    if (cs_stack_size(&cs)) {
        process_result_t o = {.has_value = true, .value = cs_pop(&cs) };
        return o;
    }
    else {
        process_result_t o = {.has_value = false, .value = 0 };
        return o;
    }
}


/////////////////////////////////////////////////////////////////
// TICK /////////////////////////////////////////////////////////

void tele_tick(scene_state_t *ss, uint8_t time) {
    // inc time
    if (ss->variables.time_act) ss->variables.time += time;

    // process delays
    for (int16_t i = 0; i < DELAY_SIZE; i++) {
        if (ss->delay.time[i]) {
            ss->delay.time[i] -= time;
            if (ss->delay.time[i] <= 0) {
                // Workaround for issue #80. (0 is the signifier for "empty")
                // Setting delay.time[i] to 1 prevents delayed delay commands
                //     from seeing a perfectly-timed delay slot as empty
                //     while it's still being processed. 
                ss->delay.time[i] = 1;
                run_command(ss, &ss->delay.commands[i]);
                ss->delay.time[i] = 0;
                ss->delay.count--;
                if (ss->delay.count == 0) tele_has_delays(false);
            }
        }
    }

    // process tr pulses
    for (int16_t i = 0; i < TR_COUNT; i++) {
        if (ss->tr_pulse_timer[i]) {
            // prevent tr_pulse_timer from being greater than tr_time
            int16_t tr_time = ss->variables.tr_time[i];
            if (tr_time < 0) tr_time = 0;
            if (ss->tr_pulse_timer[i] > tr_time)
                ss->tr_pulse_timer[i] = tr_time;

            ss->tr_pulse_timer[i] -= time;

            if (ss->tr_pulse_timer[i] <= 0) {
                ss->tr_pulse_timer[i] = 0;
                ss->variables.tr[i] = ss->variables.tr_pol[i] == 0;
                tele_tr(i, ss->variables.tr[i]);
            }
        }
    }
}

/////////////////////////////////////////////////////////////////
// ERROR MESSAGES ///////////////////////////////////////////////

const char *tele_error(error_t e) {
    const char *error_string[] = { "OK",
                                   "UNKNOWN WORD",
                                   "COMMAND TOO LONG",
                                   "NOT ENOUGH PARAMS",
                                   "TOO MANY PARAMS",
                                   "MOD NOT ALLOWED HERE",
                                   "EXTRA PRE SEPARATOR",
                                   "NEED PRE SEPARATOR",
                                   "BAD PRE SEPARATOR",
                                   "NO SUB SEP IN PRE",
                                   "MOVE LEFT",
                                   "NEED SPACE AFTER :",
                                   "NEED SPACE AFTER ;" };

    return error_string[e];
}
