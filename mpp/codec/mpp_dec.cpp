/*
 * Copyright 2015 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define  MODULE_TAG "mpp_dec"

#include <string.h>

#include "mpp_env.h"
#include "mpp_mem.h"
#include "mpp_log.h"
#include "mpp_time.h"
#include "mpp_common.h"

#include "mpp.h"
#include "mpp_frame.h"
#include "mpp_buffer_impl.h"
#include "mpp_packet_impl.h"
#include "mpp_frame_impl.h"

#include "mpp_dec_vproc.h"

static RK_U32 mpp_dec_debug = 0;

#define MPP_DEC_DBG_FUNCTION            (0x00000001)
#define MPP_DEC_DBG_STATUS              (0x00000010)
#define MPP_DEC_DBG_DETAIL              (0x00000020)
#define MPP_DEC_DBG_RESET               (0x00000040)

#define mpp_dec_dbg(flag, fmt, ...)     _mpp_dbg(mpp_dec_debug, flag, fmt, ## __VA_ARGS__)
#define mpp_dec_dbg_f(flag, fmt, ...)   _mpp_dbg_f(mpp_dec_debug, flag, fmt, ## __VA_ARGS__)

#define dec_dbg_func(fmt, ...)          mpp_dec_dbg_f(MPP_DEC_DBG_FUNCTION, fmt, ## __VA_ARGS__)
#define dec_dbg_stauts(fmt, ...)        mpp_dec_dbg(MPP_DEC_DBG_STATUS, fmt, ## __VA_ARGS__)
#define dec_dbg_detail(fmt, ...)        mpp_dec_dbg(MPP_DEC_DBG_DETAIL, fmt, ## __VA_ARGS__)
#define dec_dbg_reset(fmt, ...)         mpp_dec_dbg(MPP_DEC_DBG_RESET, fmt, ## __VA_ARGS__)

typedef union PaserTaskWait_u {
    RK_U32          val;
    struct {
        RK_U32      task_hnd     : 1;
        RK_U32      dec_pkt_idx  : 1;
        RK_U32      dec_pkt_buf  : 1;
        RK_U32      prev_task    : 1;
        RK_U32      info_change  : 1;
        RK_U32      dec_pic_buf  : 1;
        RK_U32      dec_slot_idx : 1;
        RK_U32      dis_que_full : 1;
        RK_U32      dec_all_done : 1;
    };
} PaserTaskWait;

typedef union DecTaskStatus_u {
    RK_U32          val;
    struct {
        RK_U32      task_hnd_rdy      : 1;
        RK_U32      mpp_pkt_in_rdy    : 1;
        RK_U32      dec_pkt_idx_rdy   : 1;
        RK_U32      dec_pkt_buf_rdy   : 1;
        RK_U32      task_valid_rdy    : 1;
        RK_U32      dec_pkt_copy_rdy  : 1;
        RK_U32      prev_task_rdy     : 1;
        RK_U32      info_task_gen_rdy : 1;
        RK_U32      curr_task_rdy     : 1;
        RK_U32      task_parsed_rdy   : 1;
    };
} DecTaskStatus;

typedef struct DecTask_t {
    HalTaskHnd      hnd;

    DecTaskStatus   status;
    PaserTaskWait   wait;

    RK_S32          hal_pkt_idx_in;
    RK_S32          hal_frm_idx_out;

    MppBuffer       hal_pkt_buf_in;
    MppBuffer       hal_frm_buf_out;

    HalTaskInfo     info;
} DecTask;

static void dec_task_init(DecTask *task)
{
    task->hnd = NULL;
    task->status.val = 0;
    task->wait.val   = 0;
    task->status.prev_task_rdy  = 1;

    task->hal_pkt_idx_in  = -1;
    task->hal_frm_idx_out = -1;

    task->hal_pkt_buf_in  = NULL;
    task->hal_frm_buf_out = NULL;

    hal_task_info_init(&task->info, MPP_CTX_DEC);
}

/*
 * return MPP_OK for not wait
 * return MPP_NOK for wait
 */
static MPP_RET check_task_wait(MppDec *dec, DecTask *task)
{
    if (dec->reset_flag)
        return MPP_OK;

    if (task->wait.task_hnd ||
        /* Re-check */
        (task->wait.prev_task &&
         !hal_task_check_empty(dec->tasks, TASK_PROC_DONE)) ||
        task->wait.info_change ||
        task->wait.dec_pic_buf ||
        task->wait.dis_que_full ||
        task->wait.dec_slot_idx ||
        task->wait.dec_all_done)
        return MPP_NOK;

    return MPP_OK;
}

static RK_U32 reset_dec_task(Mpp *mpp, DecTask *task)
{
    MppThread *hal      = mpp->mThreadHal;
    MppDec    *dec      = mpp->mDec;
    HalTaskGroup tasks  = dec->tasks;
    MppBufSlots frame_slots  = dec->frame_slots;
    MppBufSlots packet_slots = dec->packet_slots;
    HalDecTask *task_dec = &task->info.dec;

    dec_dbg_reset("wait hal processing\n");
    /* wait hal thread reset done here */
    while (hal_task_check_empty(tasks, TASK_PROCESSING)) {
        AutoMutex autolock(hal->mutex(THREAD_CONTROL));
        dec->hal_reset_done = 0;
        hal->lock();
        hal->signal();
        hal->unlock();
        hal->wait(THREAD_CONTROL);
    }

    dec_dbg_reset("wait hal proc done\n");

    while (hal_task_check_empty(tasks, TASK_PROC_DONE)) {
        HalTaskHnd tmp = NULL;
        hal_task_get_hnd(tasks, TASK_PROC_DONE, &tmp);
        hal_task_hnd_set_status(tmp, TASK_IDLE);
    }

    dec_dbg_reset("check hal processing empty\n");

    if (hal_task_check_empty(tasks, TASK_PROCESSING)) {
        mpp_err_f("found task not processed put %d get %d\n",
                  mpp->mTaskPutCount, mpp->mTaskGetCount);
        mpp_abort();
    }

    // do parser reset process
    {
        RK_S32 index;
        task->status.curr_task_rdy = 0;
        task->status.prev_task_rdy = 1;
        task_dec->valid = 0;
        mpp_parser_reset(dec->parser);
        mpp_hal_reset(dec->hal);
        if (dec->vproc) {
            dec_dbg_reset("reset vproc start\n");
            dec_vproc_reset(dec->vproc);
            dec_dbg_reset("reset vproc done\n");
        }

        // wait hal thread reset ready
        if (task->wait.info_change) {
            mpp_log("reset at info change status\n");
            mpp_buf_slot_reset(frame_slots, task_dec->output);
        }

        if (task->status.task_parsed_rdy) {
            mpp_log("task no send to hal que must clr current frame hal status\n");
            mpp_buf_slot_clr_flag(frame_slots, task_dec->output, SLOT_HAL_OUTPUT);
            for (RK_U32 i = 0; i < MPP_ARRAY_ELEMS(task_dec->refer); i++) {
                index = task_dec->refer[i];
                if (index >= 0)
                    mpp_buf_slot_clr_flag(frame_slots, index, SLOT_HAL_INPUT);
            }
        }

        if (dec->mpp_pkt_in) {
            mpp_packet_deinit(&dec->mpp_pkt_in);
            mpp->mPacketGetCount++;
            dec->mpp_pkt_in = NULL;
        }

        while (MPP_OK == mpp_buf_slot_dequeue(frame_slots, &index, QUEUE_DISPLAY)) {
            /* release extra ref in slot's MppBuffer */
            MppBuffer buffer = NULL;
            mpp_buf_slot_get_prop(frame_slots, index, SLOT_BUFFER, &buffer);
            if (buffer)
                mpp_buffer_put(buffer);
            mpp_buf_slot_clr_flag(frame_slots, index, SLOT_QUEUE_USE);
        }

        if (dec->use_preset_time_order) {
            mpp->mTimeStamps->flush();
        }

        if (task->status.dec_pkt_copy_rdy) {
            mpp_buf_slot_clr_flag(packet_slots, task_dec->input,  SLOT_HAL_INPUT);
            task->status.dec_pkt_copy_rdy = 0;
            task_dec->input = -1;
        }

        task->status.task_parsed_rdy = 0;
    }

    dec_task_init(task);

    return MPP_OK;
}

/* Overall mpp_dec output frame function */
static void mpp_dec_put_frame(Mpp *mpp, RK_S32 index, HalDecTaskFlag flags)
{
    MppDec *dec = mpp->mDec;
    MppBufSlots slots = dec->frame_slots;
    MppFrame frame = NULL;
    RK_U32 eos = flags.eos;
    RK_U32 change = flags.info_change;
    RK_U32 error = flags.had_error;
    RK_U32 refer = flags.used_for_ref;
    RK_U32 fake_frame = 0;

    if (index >= 0) {
        mpp_buf_slot_get_prop(slots, index, SLOT_FRAME_PTR, &frame);
        if (mpp_frame_get_mode(frame) && dec->enable_deinterlace &&
            NULL == dec->vproc) {
            dec_vproc_init(&dec->vproc, mpp);
            dec_vproc_start(dec->vproc);
        }
    } else {
        // when post-process is needed and eos without slot index case
        // we need to create a slot index for it
        mpp_assert(eos);
        mpp_assert(!change);

        if (dec->vproc) {
            mpp_buf_slot_get_unused(slots, &index);
            mpp_buf_slot_default_info(slots, index, &frame);
            mpp_buf_slot_set_flag(slots, index, SLOT_CODEC_READY);
        } else {
            mpp_frame_init(&frame);
            fake_frame = 1;
            index = 0;
        }

        mpp_frame_set_eos(frame, eos);
    }

    mpp_assert(index >= 0);
    mpp_assert(frame);

    if (mpp->mDec->disable_error) {
        mpp_frame_set_errinfo(frame, 0);
        mpp_frame_set_discard(frame, 0);
    }

    if (change) {
        /* NOTE: Set codec ready here for dequeue/enqueue */
        mpp_buf_slot_set_flag(slots, index, SLOT_CODEC_READY);
    } else {
        if (dec->use_preset_time_order) {
            MppPacket pkt = NULL;
            mpp->mTimeStamps->pull(&pkt, sizeof(pkt));
            if (pkt) {
                mpp_frame_set_dts(frame, mpp_packet_get_dts(pkt));
                mpp_frame_set_pts(frame, mpp_packet_get_pts(pkt));
                mpp_packet_deinit(&pkt);
            } else
                mpp_err_f("pull out packet error.\n");
        }
    }
    mpp_frame_set_info_change(frame, change);

    if (eos) {
        mpp_frame_set_eos(frame, 1);
        if (error) {
            if (refer)
                mpp_frame_set_errinfo(frame, 1);
            else
                mpp_frame_set_discard(frame, 1);
        }
        mpp->mTimeStamps->flush();
    }

    if (dec->vproc) {
        mpp_buf_slot_set_flag(slots, index, SLOT_QUEUE_USE);
        mpp_buf_slot_enqueue(slots, index, QUEUE_DEINTERLACE);
        dec_vproc_signal(dec->vproc);
    } else {
        // direct output -> copy a new MppFrame and output
        mpp_list *list = mpp->mFrames;
        MppFrame out = NULL;

        mpp_frame_init(&out);
        mpp_frame_copy(out, frame);

        if (mpp_debug & MPP_DBG_PTS)
            mpp_log("output frame pts %lld\n", mpp_frame_get_pts(out));

        list->lock();
        list->add_at_tail(&out, sizeof(out));
        mpp->mFramePutCount++;
        list->signal();
        list->unlock();

        if (fake_frame)
            mpp_frame_deinit(&frame);
    }
}

static void mpp_dec_push_display(Mpp *mpp, HalDecTaskFlag flags)
{
    RK_S32 index = -1;
    MppDec *dec = mpp->mDec;
    MppBufSlots frame_slots = dec->frame_slots;
    RK_U32 eos = flags.eos;
    HalDecTaskFlag tmp = flags;
    tmp.eos = 0;

    mpp->mThreadHal->lock(THREAD_OUTPUT);
    while (MPP_OK == mpp_buf_slot_dequeue(frame_slots, &index, QUEUE_DISPLAY)) {
        /* deal with current frame */
        if (eos && mpp_slots_is_empty(frame_slots, QUEUE_DISPLAY))
            tmp.eos = 1;

        mpp_dec_put_frame(mpp, index, tmp);
        mpp_buf_slot_clr_flag(frame_slots, index, SLOT_QUEUE_USE);
    }
    mpp->mThreadHal->unlock(THREAD_OUTPUT);
}

static void mpp_dec_put_task(Mpp *mpp, DecTask *task)
{
    hal_task_hnd_set_info(task->hnd, &task->info);
    mpp->mThreadHal->lock();
    hal_task_hnd_set_status(task->hnd, TASK_PROCESSING);
    mpp->mTaskPutCount++;
    mpp->mThreadHal->signal();
    mpp->mThreadHal->unlock();
    task->hnd = NULL;
}

static MPP_RET try_proc_dec_task(Mpp *mpp, DecTask *task)
{
    MppDec *dec = mpp->mDec;
    HalTaskGroup tasks  = dec->tasks;
    MppBufSlots frame_slots = dec->frame_slots;
    MppBufSlots packet_slots = dec->packet_slots;
    HalDecTask  *task_dec = &task->info.dec;
    MppBuffer hal_buf_in = NULL;
    MppBuffer hal_buf_out = NULL;
    size_t stream_size = 0;
    RK_S32 output = 0;

    /*
     * 1. get task handle from hal for parsing one frame
     */
    if (!task->hnd) {
        hal_task_get_hnd(tasks, TASK_IDLE, &task->hnd);
        if (task->hnd) {
            task->wait.task_hnd = 0;
        } else {
            task->wait.task_hnd = 1;
            return MPP_NOK;
        }
    }

    /*
     * 2. get packet for parser preparing
     */
    if (!dec->mpp_pkt_in && !task->status.curr_task_rdy) {
        MppQueue *packets = mpp->mPackets;

        if (packets->pull(&dec->mpp_pkt_in, sizeof(dec->mpp_pkt_in)))
            return MPP_NOK;
        if (dec->use_preset_time_order) {
            MppPacket pkt_in = NULL;
            mpp_packet_new(&pkt_in);
            if (pkt_in) {
                mpp_packet_set_pts(pkt_in, mpp_packet_get_pts(dec->mpp_pkt_in));
                mpp_packet_set_dts(pkt_in, mpp_packet_get_dts(dec->mpp_pkt_in));
                mpp->mTimeStamps->push(&pkt_in, sizeof(pkt_in));
            }
        }
        mpp->mPacketGetCount++;
    }

    /*
     * 3. send packet data to parser for prepare
     *
     *    mpp_parser_prepare functioin input / output
     *    input:    input MppPacket data from user
     *    output:   one packet which contains one frame for hardware processing
     *              output information will be stored in task_dec->input_packet
     *              output data can be stored inside of parser.
     *
     *    NOTE:
     *    1. Prepare process is controlled by need_split flag
     *       If need_split flag is zero prepare function is just copy the input
     *       packet to task_dec->input_packet
     *       If need_split flag is non-zero prepare function will call split funciton
     *       of different coding type and find the start and end of one frame. Then
     *       copy data to task_dec->input_packet
     *    2. On need_split mode if one input MppPacket contain multiple frame for
     *       decoding one mpp_parser_prepare call will only frame for task. Then input
     *       MppPacket->pos/length will be updated. The input MppPacket will not be
     *       released until it is totally consumed.
     *    3. On spliting frame if one frame contain multiple slices and these multiple
     *       slices have different pts/dts the output frame will use the last pts/dts
     *       as the output frame's pts/dts.
     *
     */
    if (!task->status.curr_task_rdy) {
        if (mpp_debug & MPP_DBG_PTS)
            mpp_log("input packet pts %lld\n",
                    mpp_packet_get_pts(dec->mpp_pkt_in));

        mpp_parser_prepare(dec->parser, dec->mpp_pkt_in, task_dec);

        if (0 == mpp_packet_get_length(dec->mpp_pkt_in)) {
            mpp_packet_deinit(&dec->mpp_pkt_in);
            dec->mpp_pkt_in = NULL;
        }
    }

    task->status.curr_task_rdy = task_dec->valid;
    /*
    * We may find eos in prepare step and there will be no anymore vaild task generated.
    * So here we try push eos task to hal, hal will push all frame to display then
    * push a eos frame to tell all frame decoded
    */
    if (task_dec->flags.eos && !task_dec->valid)
        mpp_dec_put_task(mpp, task);

    if (!task->status.curr_task_rdy)
        return MPP_NOK;

    // NOTE: packet in task should be ready now
    mpp_assert(task_dec->input_packet);

    /*
     * 4. look for a unused packet slot index
     */
    if (task_dec->input < 0) {
        mpp_buf_slot_get_unused(packet_slots, &task_dec->input);
    }

    task->wait.dec_pkt_idx = (task_dec->input < 0);
    if (task->wait.dec_pkt_idx)
        return MPP_NOK;

    /*
     * 5. malloc hardware buffer for the packet slot index
     */
    task->hal_pkt_idx_in = task_dec->input;
    stream_size = mpp_packet_get_size(task_dec->input_packet);

    mpp_buf_slot_get_prop(packet_slots, task->hal_pkt_idx_in, SLOT_BUFFER, &hal_buf_in);
    if (NULL == hal_buf_in) {
        mpp_buffer_get(mpp->mPacketGroup, &hal_buf_in, stream_size);
        if (hal_buf_in) {
            mpp_buf_slot_set_prop(packet_slots, task->hal_pkt_idx_in, SLOT_BUFFER, hal_buf_in);
            mpp_buffer_put(hal_buf_in);
        }
    } else {
        MppBufferImpl *buf = (MppBufferImpl *)hal_buf_in;
        mpp_assert(buf->info.size >= stream_size);
    }

    task->hal_pkt_buf_in = hal_buf_in;
    task->wait.dec_pkt_buf = (NULL == hal_buf_in);
    if (task->wait.dec_pkt_buf)
        return MPP_NOK;

    /*
     * 6. copy prepared stream to hardware buffer
     */
    if (!task->status.dec_pkt_copy_rdy) {
        void *dst = mpp_buffer_get_ptr(task->hal_pkt_buf_in);
        void *src = mpp_packet_get_data(task_dec->input_packet);
        size_t length = mpp_packet_get_length(task_dec->input_packet);
        memcpy(dst, src, length);
        mpp_buf_slot_set_flag(packet_slots, task_dec->input, SLOT_CODEC_READY);
        mpp_buf_slot_set_flag(packet_slots, task_dec->input, SLOT_HAL_INPUT);
        task->status.dec_pkt_copy_rdy = 1;
    }

    /* 7.1 if not fast mode wait previous task done here */
    if (!dec->parser_fast_mode) {
        // wait previous task done
        if (!task->status.prev_task_rdy) {
            HalTaskHnd task_prev = NULL;
            hal_task_get_hnd(tasks, TASK_PROC_DONE, &task_prev);
            if (task_prev) {
                task->status.prev_task_rdy  = 1;
                task->wait.prev_task = 0;
                hal_task_hnd_set_status(task_prev, TASK_IDLE);
                task_prev = NULL;
            } else {
                task->wait.prev_task = 1;
                return MPP_NOK;
            }
        }
    } else if (task->wait.dec_all_done) {
        if (!hal_task_check_empty(dec->tasks, TASK_PROCESSING)) {
            task->wait.dec_all_done = 0;
        } else {
            return MPP_NOK;
        }
    }

    dec_dbg_detail("check prev task pass\n");

    /* too many frame delay in dispaly queue */
    if (mpp->mFrames) {
        task->wait.dis_que_full = (mpp->mFrames->list_size() > 4) ? 1 : 0;
        if (task->wait.dis_que_full)
            return MPP_ERR_DISPLAY_FULL;
    }
    dec_dbg_detail("check mframes pass\n");

    /* 7.2 look for a unused hardware buffer for output */
    if (mpp->mFrameGroup) {
        RK_S32 unused = mpp_buffer_group_unused(mpp->mFrameGroup);

        // NOTE: When dec post-process is enabled reserve 2 buffer for it.
        task->wait.dec_pic_buf = (dec->vproc) ? (unused < 3) : (unused < 1);
        if (task->wait.dec_pic_buf)
            return MPP_ERR_BUFFER_FULL;
    }
    dec_dbg_detail("check frame group count pass\n");

    /* 7.3 wait for a unused slot index for decoder parse operation */
    task->wait.dec_slot_idx = (mpp_slots_get_unused_count(frame_slots)) ? (0) : (1);
    if (task->wait.dec_slot_idx)
        return MPP_ERR_BUFFER_FULL;

    /*
     * 8. send packet data to parser
     *
     *    parser prepare functioin input / output
     *    input:    packet data
     *    output:   dec task output information (with dxva output slot)
     *              buffer slot usage informatioin
     *
     *    NOTE:
     *    1. dpb slot will be set internally in parser process.
     *    2. parse function need to set valid flag when one frame is ready.
     *    3. if packet size is zero then next packet is needed.
     *    4. detect whether output index has MppBuffer and task valid
     */
    if (!task->status.task_parsed_rdy) {
        mpp_parser_parse(dec->parser, task_dec);
        task->status.task_parsed_rdy = 1;
    }

    if (task_dec->output < 0 || !task_dec->valid) {
        /*
         * We may meet an eos in parser step and there will be no anymore vaild
         * task generated. So here we try push eos task to hal, hal will push
         * all frame(s) to display, a frame of them with a eos flag will be
         * used to inform that all frame have decoded
         */
        if (task_dec->flags.eos) {
            mpp_dec_put_task(mpp, task);
        } else {
            hal_task_hnd_set_status(task->hnd, TASK_IDLE);
            task->hnd = NULL;
        }

        if (task->status.dec_pkt_copy_rdy) {
            mpp_buf_slot_clr_flag(packet_slots, task_dec->input,  SLOT_HAL_INPUT);
            task->status.dec_pkt_copy_rdy = 0;
        }
        task->status.curr_task_rdy  = 0;
        task->status.task_parsed_rdy = 0;
        hal_task_info_init(&task->info, MPP_CTX_DEC);
        return MPP_NOK;
    }
    dec_dbg_detail("check output index pass\n");

    /*
     * 9. parse local task and slot to check whether new buffer or info change is needed.
     *
     * detect info change from frame slot
     */
    if (mpp_buf_slot_is_changed(frame_slots)) {
        if (!task->status.info_task_gen_rdy) {
            task_dec->flags.info_change = 1;
            mpp_dec_put_task(mpp, task);
            task->status.info_task_gen_rdy = 1;
            return MPP_ERR_STREAM;
        }
    }

    task->wait.info_change = mpp_buf_slot_is_changed(frame_slots);
    if (task->wait.info_change) {
        return MPP_ERR_STREAM;
    } else {
        task->status.info_task_gen_rdy = 0;
        task_dec->flags.info_change = 0;
        // NOTE: check the task must be ready
        mpp_assert(task->hnd);
    }

    /* 10. whether the frame buffer group is internal or external */
    if (NULL == mpp->mFrameGroup) {
        mpp_log("mpp_dec use internal frame buffer group\n");
        mpp_buffer_group_get_internal(&mpp->mFrameGroup, MPP_BUFFER_TYPE_ION);
    }

    /*
     * 11. do buffer operation according to usage information
     *
     *    possible case:
     *    a. normal case
     *       - wait and alloc a normal frame buffer
     *    b. field mode case
     *       - two field may reuse a same buffer, no need to alloc
     *    c. info change case
     *       - need buffer in different side, need to send a info change
     *         frame to hal loop.
     */
    output = task_dec->output;
    mpp_buf_slot_get_prop(frame_slots, output, SLOT_BUFFER, &hal_buf_out);
    if (NULL == hal_buf_out) {
        size_t size = mpp_buf_slot_get_size(frame_slots);
        mpp_buffer_get(mpp->mFrameGroup, &hal_buf_out, size);
        if (hal_buf_out)
            mpp_buf_slot_set_prop(frame_slots, output, SLOT_BUFFER, hal_buf_out);

    }

    dec_dbg_detail("check output buffer %p\n", hal_buf_out);

    task->hal_frm_buf_out = hal_buf_out;
    task->wait.dec_pic_buf = (NULL == hal_buf_out);
    if (task->wait.dec_pic_buf)
        return MPP_NOK;

    /* generating registers table */
    mpp_hal_reg_gen(dec->hal, &task->info);

    /* send current register set to hardware */
    mpp_hal_hw_start(dec->hal, &task->info);

    /*
     * 12. send dxva output information and buffer information to hal thread
     *    combinate video codec dxva output and buffer information
     */
    mpp_dec_put_task(mpp, task);

    task->wait.dec_all_done = task_dec->flags.wait_done;
    task->status.dec_pkt_copy_rdy  = 0;
    task->status.curr_task_rdy  = 0;
    task->status.task_parsed_rdy = 0;
    task->status.prev_task_rdy   = 0;
    hal_task_info_init(&task->info, MPP_CTX_DEC);

    return MPP_OK;
}

void *mpp_dec_parser_thread(void *data)
{
    Mpp *mpp = (Mpp*)data;
    MppThread *parser   = mpp->mThreadCodec;
    MppDec    *dec      = mpp->mDec;
    MppBufSlots packet_slots = dec->packet_slots;

    DecTask task;
    HalDecTask  *task_dec = &task.info.dec;

    dec_task_init(&task);

    while (1) {
        {
            AutoMutex autolock(parser->mutex());
            if (MPP_THREAD_RUNNING != parser->get_status())
                break;

            /*
             * parser thread need to wait at cases below:
             * 1. no task slot for output
             * 2. no packet for parsing
             * 3. info change on progress
             * 3. no buffer on analyzing output task
             */
            dec_dbg_stauts("%p wait status: 0x%08x\n", dec, task.wait.val);
            if (check_task_wait(dec, &task))
                parser->wait();

            dec_dbg_stauts("%p done status: 0x%08x\n", dec, task.wait.val);
        }

        if (dec->reset_flag) {
            reset_dec_task(mpp, &task);

            AutoMutex autolock(parser->mutex(THREAD_CONTROL));
            dec->hal_reset_done = 0;
            dec->reset_flag = 0;
            parser->signal(THREAD_CONTROL);
            continue;
        }

        if (try_proc_dec_task(mpp, &task))
            continue;

    }

    mpp_dbg(MPP_DBG_INFO, "mpp_dec_parser_thread is going to exit\n");
    if (NULL != task.hnd && task_dec->valid) {
        mpp_buf_slot_set_flag(packet_slots, task_dec->input, SLOT_CODEC_READY);
        mpp_buf_slot_set_flag(packet_slots, task_dec->input, SLOT_HAL_INPUT);
        mpp_buf_slot_clr_flag(packet_slots, task_dec->input, SLOT_HAL_INPUT);
    }
    mpp_buffer_group_clear(mpp->mPacketGroup);
    mpp_dbg(MPP_DBG_INFO, "mpp_dec_parser_thread exited\n");
    return NULL;
}

void *mpp_dec_hal_thread(void *data)
{
    Mpp *mpp = (Mpp*)data;
    MppThread *hal      = mpp->mThreadHal;
    MppDec    *dec      = mpp->mDec;
    HalTaskGroup tasks  = dec->tasks;
    MppBufSlots frame_slots = dec->frame_slots;
    MppBufSlots packet_slots = dec->packet_slots;

    HalTaskHnd  task = NULL;
    HalTaskInfo task_info;
    HalDecTask  *task_dec = &task_info.dec;

    while (1) {
        /* hal thread wait for dxva interface intput first */
        {
            AutoMutex work_lock(hal->mutex());
            if (MPP_THREAD_RUNNING != hal->get_status())
                break;

            if (hal_task_get_hnd(tasks, TASK_PROCESSING, &task)) {
                // process all task then do reset process
                if (dec->reset_flag && !dec->hal_reset_done) {
                    AutoMutex ctrl_lock(hal->mutex(THREAD_CONTROL));
                    dec->hal_reset_done = 1;
                    hal->signal(THREAD_CONTROL);
                    continue;
                }

                hal->wait();
                continue;
            }
        }

        if (task) {
            mpp->mTaskGetCount++;

            hal_task_hnd_get_info(task, &task_info);

            /*
             * check info change flag
             * if this is a frame with that flag, only output an empty
             * MppFrame without any image data for info change.
             */
            if (task_dec->flags.info_change) {
                mpp_dec_flush(dec);
                mpp_dec_push_display(mpp, task_dec->flags);
                mpp_dec_put_frame(mpp, task_dec->output, task_dec->flags);

                hal_task_hnd_set_status(task, TASK_IDLE);
                task = NULL;
                mpp->mThreadCodec->signal();
                continue;
            }
            /*
             * check eos task
             * if this task is invalid while eos flag is set, we will
             * flush display queue then push the eos frame to info that
             * all frames have decoded.
             */
            if (task_dec->flags.eos && !task_dec->valid) {
                mpp_dec_push_display(mpp, task_dec->flags);
                /*
                 * Use -1 as invalid buffer slot index.
                 * Reason: the last task maybe is a empty task with eos flag
                 * only but this task may go through vproc process also. We need
                 * create a buffer slot index for it.
                 */
                mpp_dec_put_frame(mpp, -1, task_dec->flags);

                hal_task_hnd_set_status(task, TASK_IDLE);
                mpp->mThreadCodec->signal();
                task = NULL;
                continue;
            }

            mpp_hal_hw_wait(dec->hal, &task_info);
            /*
             * when hardware decoding is done:
             * 1. clear decoding flag (mark buffer is ready)
             * 2. use get_display to get a new frame with buffer
             * 3. add frame to output list
             * repeat 2 and 3 until not frame can be output
             */
            mpp_buf_slot_clr_flag(packet_slots, task_dec->input,
                                  SLOT_HAL_INPUT);

            hal_task_hnd_set_status(task, (dec->parser_fast_mode) ?
                                    (TASK_IDLE) : (TASK_PROC_DONE));
            mpp->mThreadCodec->signal();
            task = NULL;

            mpp_buf_slot_clr_flag(frame_slots, task_dec->output, SLOT_HAL_OUTPUT);
            for (RK_U32 i = 0; i < MPP_ARRAY_ELEMS(task_dec->refer); i++) {
                RK_S32 index = task_dec->refer[i];
                if (index >= 0)
                    mpp_buf_slot_clr_flag(frame_slots, index, SLOT_HAL_INPUT);
            }
            if (task_dec->flags.eos)
                mpp_dec_flush(dec);
            mpp_dec_push_display(mpp, task_dec->flags);
        }
    }

    mpp_assert(mpp->mTaskPutCount == mpp->mTaskGetCount);
    mpp_dbg(MPP_DBG_INFO, "mpp_dec_hal_thread exited\n");
    return NULL;
}

static MPP_RET dec_release_task_in_port(MppPort port)
{
    MPP_RET ret = MPP_OK;
    MppPacket packet = NULL;
    MppFrame frame = NULL;
    MppTask mpp_task;

    do {
        ret = mpp_port_dequeue(port, &mpp_task);
        if (ret || mpp_task == NULL)
            break;

        packet = NULL;
        frame = NULL;
        ret = mpp_task_meta_get_frame(mpp_task, KEY_OUTPUT_FRAME,  &frame);
        if (frame) {
            mpp_frame_deinit(&frame);
            frame = NULL;
        }
        ret = mpp_task_meta_get_packet(mpp_task, KEY_INPUT_PACKET, &packet);
        if (packet) {
            mpp_packet_deinit(&packet);
            packet = NULL;
        }

        mpp_port_enqueue(port, mpp_task);
        mpp_task = NULL;
    } while (1);

    return ret;
}

void *mpp_dec_advanced_thread(void *data)
{
    Mpp *mpp = (Mpp*)data;
    MppDec *dec      = mpp->mDec;
    MppBufSlots frame_slots = dec->frame_slots;
    MppBufSlots packet_slots = dec->packet_slots;
    MppThread *thd_dec  = mpp->mThreadCodec;
    DecTask task;   /* decoder task */
    DecTask *pTask = &task;
    dec_task_init(pTask);
    HalDecTask  *task_dec = &pTask->info.dec;

    MppPort input  = mpp_task_queue_get_port(mpp->mInputTaskQueue,  MPP_PORT_OUTPUT);
    MppPort output = mpp_task_queue_get_port(mpp->mOutputTaskQueue, MPP_PORT_INPUT);
    MppTask mpp_task = NULL;
    MPP_RET ret = MPP_OK;
    MppFrame frame = NULL;
    MppPacket packet = NULL;

    while (1) {
        {
            AutoMutex autolock(thd_dec->mutex());
            if (MPP_THREAD_RUNNING == thd_dec->get_status()) {
                ret = mpp_port_dequeue(input, &mpp_task);
                if (ret || NULL == mpp_task) {
                    thd_dec->wait();
                }
            } else {
                break;
            }
        }

        if (mpp_task != NULL) {
            mpp_task_meta_get_packet(mpp_task, KEY_INPUT_PACKET, &packet);
            mpp_task_meta_get_frame (mpp_task, KEY_OUTPUT_FRAME,  &frame);

            if (NULL == packet) {
                mpp_port_enqueue(input, mpp_task);
                continue;
            }

            if (mpp_packet_get_buffer(packet)) {
                /*
                 * if there is available buffer in the input packet do decoding
                 */
                MppBuffer input_buffer = mpp_packet_get_buffer(packet);
                MppBuffer output_buffer = mpp_frame_get_buffer(frame);

                mpp_parser_prepare(dec->parser, packet, task_dec);

                /*
                 * We may find eos in prepare step and there will be no anymore vaild task generated.
                 * So here we try push eos task to hal, hal will push all frame to display then
                 * push a eos frame to tell all frame decoded
                 */
                if (task_dec->flags.eos && !task_dec->valid) {
                    mpp_frame_set_eos(frame, 1);
                    goto DEC_OUT;
                }

                /*
                 *  look for a unused packet slot index
                 */
                if (task_dec->input < 0) {
                    mpp_buf_slot_get_unused(packet_slots, &task_dec->input);
                }
                mpp_buf_slot_set_prop(packet_slots, task_dec->input, SLOT_BUFFER, input_buffer);
                mpp_buf_slot_set_flag(packet_slots, task_dec->input, SLOT_CODEC_READY);
                mpp_buf_slot_set_flag(packet_slots, task_dec->input, SLOT_HAL_INPUT);

                ret = mpp_parser_parse(dec->parser, task_dec);
                if (ret != MPP_OK) {
                    mpp_err_f("something wrong with mpp_parser_parse!\n");
                    mpp_frame_set_errinfo(frame, 1); /* 0 - OK; 1 - error */
                    mpp_buf_slot_clr_flag(packet_slots, task_dec->input,  SLOT_HAL_INPUT);
                    goto DEC_OUT;
                }

                if (mpp_buf_slot_is_changed(frame_slots)) {
                    size_t slot_size = mpp_buf_slot_get_size(frame_slots);
                    size_t buffer_size = mpp_buffer_get_size(output_buffer);

                    if (slot_size == buffer_size) {
                        mpp_buf_slot_ready(frame_slots);
                    }

                    if (slot_size != buffer_size) {
                        mpp_err_f("slot size %d is not equal to buffer size %d\n",
                                  slot_size, buffer_size);
                        mpp_assert(slot_size == buffer_size);
                    }
                }

                mpp_buf_slot_set_prop(frame_slots, task_dec->output, SLOT_BUFFER, output_buffer);

                // register genertation
                mpp_hal_reg_gen(dec->hal, &pTask->info);
                mpp_hal_hw_start(dec->hal, &pTask->info);
                mpp_hal_hw_wait(dec->hal, &pTask->info);

                MppFrame tmp = NULL;
                mpp_buf_slot_get_prop(frame_slots, task_dec->output, SLOT_FRAME_PTR, &tmp);
                mpp_frame_set_width(frame, mpp_frame_get_width(tmp));
                mpp_frame_set_height(frame, mpp_frame_get_height(tmp));
                mpp_frame_set_hor_stride(frame, mpp_frame_get_hor_stride(tmp));
                mpp_frame_set_ver_stride(frame, mpp_frame_get_ver_stride(tmp));
                mpp_frame_set_pts(frame, mpp_frame_get_pts(tmp));
                mpp_frame_set_fmt(frame, mpp_frame_get_fmt(tmp));
                mpp_frame_set_errinfo(frame, mpp_frame_get_errinfo(tmp));

                mpp_buf_slot_clr_flag(packet_slots, task_dec->input,  SLOT_HAL_INPUT);
                mpp_buf_slot_clr_flag(frame_slots, task_dec->output, SLOT_HAL_OUTPUT);
            } else {
                /*
                 * else init a empty frame for output
                 */
                mpp_log_f("line(%d): Error! Get no buffer from input packet\n", __LINE__);
                mpp_frame_init(&frame);
                mpp_frame_set_errinfo(frame, 1);
            }

            /*
             * first clear output packet
             * then enqueue task back to input port
             * final user will release the mpp_frame they had input
             */
        DEC_OUT:
            mpp_task_meta_set_packet(mpp_task, KEY_INPUT_PACKET, packet);
            mpp_port_enqueue(input, mpp_task);
            mpp_task = NULL;

            // send finished task to output port
            mpp_port_poll(output, MPP_POLL_BLOCK);
            mpp_port_dequeue(output, &mpp_task);
            mpp_task_meta_set_frame(mpp_task, KEY_OUTPUT_FRAME, frame);

            // setup output task here
            mpp_port_enqueue(output, mpp_task);
            mpp_task = NULL;
            packet = NULL;
            frame = NULL;

            hal_task_info_init(&pTask->info, MPP_CTX_DEC);
        }
    }

    // clear remain task in output port
    dec_release_task_in_port(input);
    dec_release_task_in_port(mpp->mOutputPort);

    return NULL;
}

MPP_RET mpp_dec_init(MppDec **dec, MppDecCfg *cfg)
{
    MPP_RET ret;
    MppCodingType coding;
    MppBufSlots frame_slots = NULL;
    MppBufSlots packet_slots = NULL;
    Parser parser = NULL;
    MppHal hal = NULL;
    RK_S32 hal_task_count = 0;
    MppDec *p = NULL;
    IOInterruptCB cb = {NULL, NULL};

    dec_dbg_func("in\n");
    mpp_env_get_u32("mpp_dec_debug", &mpp_dec_debug, 0);

    if (NULL == dec || NULL == cfg) {
        mpp_err_f("invalid input dec %p cfg %p\n", dec, cfg);
        return MPP_ERR_NULL_PTR;
    }

    *dec = NULL;

    p = mpp_calloc(MppDec, 1);
    if (NULL == p) {
        mpp_err_f("failed to malloc context\n");
        return MPP_ERR_MALLOC;
    }

    coding = cfg->coding;
    hal_task_count = (cfg->fast_mode) ? (3) : (2);

    do {
        ret = mpp_buf_slot_init(&frame_slots);
        if (ret) {
            mpp_err_f("could not init frame buffer slot\n");
            break;
        }

        ret = mpp_buf_slot_init(&packet_slots);
        if (ret) {
            mpp_err_f("could not init packet buffer slot\n");
            break;
        }

        mpp_buf_slot_setup(packet_slots, hal_task_count);
        cb.callBack = mpp_dec_notify;
        cb.opaque = p;
        ParserCfg parser_cfg = {
            coding,
            frame_slots,
            packet_slots,
            hal_task_count,
            cfg->need_split,
            cfg->internal_pts,
            cb,
        };

        ret = mpp_parser_init(&parser, &parser_cfg);
        if (ret) {
            mpp_err_f("could not init parser\n");
            break;
        }
        cb.callBack = mpp_hal_callback;
        cb.opaque = parser;
        // then init hal with task count from parser
        MppHalCfg hal_cfg = {
            MPP_CTX_DEC,
            coding,
            HAL_MODE_LIBVPU,
            HAL_RKVDEC,
            frame_slots,
            packet_slots,
            NULL,
            NULL,
            NULL,
            parser_cfg.task_count,
            cfg->fast_mode,
            cb,
        };

        ret = mpp_hal_init(&hal, &hal_cfg);
        if (ret) {
            mpp_err_f("could not init hal\n");
            break;
        }

        p->coding = coding;
        p->parser = parser;
        p->hal    = hal;
        p->tasks  = hal_cfg.tasks;
        p->frame_slots  = frame_slots;
        p->packet_slots = packet_slots;

        p->mpp                  = cfg->mpp;
        p->parser_need_split    = cfg->need_split;
        p->parser_fast_mode     = cfg->fast_mode;
        p->parser_internal_pts  = cfg->internal_pts;
        p->enable_deinterlace   = 1;
        *dec = p;
        dec_dbg_func("out\n");
        return MPP_OK;
    } while (0);

    mpp_dec_deinit(p);
    return MPP_NOK;
}

MPP_RET mpp_dec_deinit(MppDec *dec)
{
    dec_dbg_func("in %p\n", dec);
    if (NULL == dec) {
        mpp_err_f("found NULL input\n");
        return MPP_ERR_NULL_PTR;
    }

    if (dec->parser) {
        mpp_parser_deinit(dec->parser);
        dec->parser = NULL;
    }

    if (dec->hal) {
        mpp_hal_deinit(dec->hal);
        dec->hal = NULL;
    }

    if (dec->vproc) {
        dec_vproc_deinit(dec->vproc);
        dec->vproc = NULL;
    }

    if (dec->frame_slots) {
        mpp_buf_slot_deinit(dec->frame_slots);
        dec->frame_slots = NULL;
    }

    if (dec->packet_slots) {
        mpp_buf_slot_deinit(dec->packet_slots);
        dec->packet_slots = NULL;
    }

    mpp_free(dec);
    dec_dbg_func("out\n");
    return MPP_OK;
}

MPP_RET mpp_dec_reset(MppDec *dec)
{
    dec_dbg_func("in %p\n", dec);
    if (NULL == dec) {
        mpp_err_f("found NULL input dec %p\n", dec);
        return MPP_ERR_NULL_PTR;
    }

    Mpp *mpp = (Mpp *)dec->mpp;
    MppThread *parser = mpp->mThreadCodec;

    if (dec->coding != MPP_VIDEO_CodingMJPEG) {
        // set reset flag
        parser->lock(THREAD_CONTROL);
        dec->reset_flag = 1;

        // signal parser thread to reset
        parser->lock();
        parser->signal();
        parser->unlock();

        parser->wait(THREAD_CONTROL);
        parser->unlock(THREAD_CONTROL);
    }

    dec_dbg_func("out\n");
    return MPP_OK;
}

MPP_RET mpp_dec_flush(MppDec *dec)
{
    dec_dbg_func("in %p\n", dec);
    if (NULL == dec) {
        mpp_err_f("found NULL input dec %p\n", dec);
        return MPP_ERR_NULL_PTR;
    }

    mpp_parser_flush(dec->parser);
    mpp_hal_flush(dec->hal);

    dec_dbg_func("out\n");
    return MPP_OK;
}

MPP_RET mpp_dec_notify(void *ctx, void *info)
{
    dec_dbg_func("in %p\n", ctx);
    (void)ctx;
    (void)info;
    dec_dbg_func("out\n");
    return MPP_OK;
}

MPP_RET mpp_dec_control(MppDec *dec, MpiCmd cmd, void *param)
{
    dec_dbg_func("in %p 0x%08x %p\n", dec, cmd, param);
    if (NULL == dec) {
        mpp_err_f("found NULL input dec %p\n", dec);
        return MPP_ERR_NULL_PTR;
    }
    mpp_parser_control(dec->parser, cmd, param);
    mpp_hal_control(dec->hal, cmd, param);

    switch (cmd) {
    case MPP_DEC_SET_FRAME_INFO : {
        MppFrame frame = (MppFrame)param;

        mpp_slots_set_prop(dec->frame_slots, SLOTS_FRAME_INFO, frame);

        mpp_log("setting default w %4d h %4d h_str %4d v_str %4d\n",
                mpp_frame_get_width(frame),
                mpp_frame_get_height(frame),
                mpp_frame_get_hor_stride(frame),
                mpp_frame_get_ver_stride(frame));

    } break;
    case MPP_DEC_GET_VPUMEM_USED_COUNT: {
        RK_S32 *p = (RK_S32 *)param;
        *p = mpp_slots_get_used_count(dec->frame_slots);
        dec_dbg_func("used count %d\n", *p);
    } break;
    case MPP_DEC_SET_DISABLE_ERROR: {
        dec->disable_error = *((RK_U32 *)param);
        dec_dbg_func("disable error %d\n", dec->disable_error);
    } break;
    case MPP_DEC_SET_PRESENT_TIME_ORDER: {
        dec->use_preset_time_order = (param) ? (*((RK_U32 *)param)) : (1);
        dec_dbg_func("preset time order %d\n", dec->use_preset_time_order);
    } break;
    case MPP_DEC_SET_ENABLE_DEINTERLACE: {
        dec->enable_deinterlace = (param) ? (*((RK_U32 *)param)) : (1);
        dec_dbg_func("enable deinterlace %d\n", dec->enable_deinterlace);
    } break;
    default : {
    } break;
    }

    dec_dbg_func("out\n");
    return MPP_OK;
}
