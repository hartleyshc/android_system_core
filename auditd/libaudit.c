/*
 * Copyright 2012, Samsung Telecommunications of America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Written by William Roberts <w.roberts@sta.samsung.com>
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#define LOG_TAG "libaudit"
#include <cutils/log.h>
#include <cutils/klog.h>

#include "libaudit.h"

/**
 * Copies the netlink message data to the reply structure.
 *
 * When the kernel sends a response back, we must adjust the response from the
 * netlink message header.
 * All the data is in rep->msg but belongs in the type enforced fields in the struct.
 *
 * @param rep
 *  The response
 * @param len
 *  The length of the message, len must never be less than 0!
 * @return
 *  This function returns 0 on success, else -error.
 */
static int set_internal_fields(struct audit_reply *rep, ssize_t len)
{
    int rc;

    /*
     * We end up setting a specific field in the union, but since it
     * is a union and they are all of type pointer, we can just clear
     * one.
     */
    rep->status = NULL;

    /* Set the response from the netlink message */
    rep->nlh = &rep->msg.nlh;
    rep->len = rep->msg.nlh.nlmsg_len;
    rep->type = rep->msg.nlh.nlmsg_type;

    /* Check if the reply from the kernel was ok */
    if (!NLMSG_OK(rep->nlh, (size_t)len)) {
        rc = (len == sizeof(rep->msg)) ? -EFBIG : -EBADE;
        SLOGE("Bad kernel response %s", strerror(-rc));
        return rc;
    }

    /* Next we'll set the data structure to point to msg.data. This is
     * to avoid having to use casts later. */
    if (rep->type == NLMSG_ERROR) {
        rep->error = NLMSG_DATA(rep->nlh);
    } else if (rep->type == AUDIT_GET) {
        rep->status = NLMSG_DATA(rep->nlh);
    } else if (rep->type == AUDIT_LIST_RULES) {
        rep->ruledata = NLMSG_DATA(rep->nlh);
    } else if (rep->type == AUDIT_SIGNAL_INFO) {
        rep->signal_info = NLMSG_DATA(rep->nlh);
    }
    /* If it is not any of the above specific events, it must be a generic message */
    else {
        rep->message = NLMSG_DATA(rep->nlh);
    }

    return 0;
}

/**
 * Waits for an ack from the kernel
 * @param fd
 *  The netlink socket fd
 * @param seq
 *  The current sequence number were acking on
 * @return
 *  This function returns 0 on success, else -errno.
 */
static int get_ack(int fd, int16_t seq)
{
    int rc;
    struct audit_reply rep;

    /* Sanity check the input, this is an internal interface this shouldn't happen */
    if (fd < 0) {
        return -EINVAL;
    }

    rc = audit_get_reply(fd, &rep, GET_REPLY_BLOCKING, MSG_PEEK);
    if (rc < 0) {
        return rc;
    }

    if (rep.type == NLMSG_ERROR) {
        audit_get_reply(fd, &rep, GET_REPLY_BLOCKING, 0);
        if (rep.error->error) {
            return -rep.error->error;
        }
    }

    if ((int16_t)rep.nlh->nlmsg_seq != seq) {
        SLOGW("Expected sequence number between user space and kernel space is out of skew, "
        "expected %u got %u", seq, rep.nlh->nlmsg_seq);
    }

    return 0;
}

/**
 *
 * @param fd
 *  The netlink socket fd
 * @param type
 *  The type of netlink message
 * @param data
 *  The data to send
 * @param size
 *  The length of the data in bytes
 * @return
 *  This function returns a positive sequence number on success, else -errno.
 */
static int audit_send(int fd, int type, const void *data, unsigned int size)
{
    int rc;
    static int16_t sequence = 0;
    struct audit_message req;
    struct sockaddr_nl addr;

    memset(&req, 0, sizeof(req));
    memset(&addr, 0, sizeof(addr));

    /* We always send netlink messaged */
    addr.nl_family = AF_NETLINK;

    /* Set up the netlink headers */
    req.nlh.nlmsg_type = type;
    req.nlh.nlmsg_len = NLMSG_SPACE(size);
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;

    /*
     * Check for a valid fd, even though sendto would catch this, its easier to always
     * blindly increment the sequence number
     */
    if (fd < 0) {
        return -EBADF;
    }

    /* Ensure the message is not too big */
    if (NLMSG_SPACE(size) > MAX_AUDIT_MESSAGE_LENGTH) {
        SLOGE("netlink message is too large");
        return -EINVAL;
    }

    /* Only memcpy in the data if it was specified */
    if (size && data)
        memcpy(NLMSG_DATA(&req.nlh), data, size);

    /*
     * Only increment the sequence number on a guarantee
     * you will send it to the kernel.
     *
     * Also, the sequence is defined as a u32 in the kernel
     * struct. Using an int here might not work on 32/64 bit splits. A
     * signed 64 bit value can overflow a u32..but a u32
     * might not fit in the response, so we need to use s32.
     * Which is still kind of hackish since int could be 16 bits
     * in size. The only safe type to use here is a signed 16
     * bit value.
     */
    req.nlh.nlmsg_seq = ++sequence;

    /* While failing and its due to interrupts */
    do {
        /* Try and send the netlink message */
        rc = sendto(fd, &req, req.nlh.nlmsg_len, 0, (struct sockaddr*) &addr, sizeof(addr));

    } while (rc < 0 && errno == EINTR);

    /* Not all the bytes were sent */
    if ((uint32_t) rc != req.nlh.nlmsg_len) {
        rc = -EPROTO;
        goto out;
    } else if (rc < 0) {
        rc = -errno;
        SLOGE("Error sending data over the netlink socket: %s", strerror(-errno));
        goto out;
    }

    /* We sent all the bytes, get the ack */
    rc = get_ack(fd, sequence);

    /* If the ack failed, return the error, else return the sequence number */
    rc = (rc == 0) ? (int) sequence : rc;

out:
    /* Don't let sequence roll to negative */
    if (sequence < 0) {
        SLOGW("Auditd to Kernel sequence number has rolled over");
        sequence = 0;
    }

    return rc;
}

int audit_set_pid(int fd, uint32_t pid, rep_wait_t wmode)
{
    int rc;
    struct audit_reply rep;
    struct audit_status status;

    memset(&status, 0, sizeof(status));

    /*
     * In order to set the auditd PID we send an audit message over the netlink socket
     * with the pid field of the status struct set to our current pid, and the
     * the mask set to AUDIT_STATUS_PID
     */
    status.pid = pid;
    status.mask = AUDIT_STATUS_PID;

    /* Let the kernel know this pid will be registering for audit events */
    rc = audit_send(fd, AUDIT_SET, &status, sizeof(status));
    if (rc < 0) {
        SLOGE("Could net set pid for audit events, error: %s", strerror(-rc));
        return rc;
    }

    /*
     * In a request where we need to wait for a response, wait for the message
     * and discard it. This message confirms and sync's us with the kernel.
     * This daemon is now registered as the audit logger. Only wait if the
     * wmode is != WAIT_NO
     */
    if (wmode != WAIT_NO) {
        /* TODO
         * If the daemon dies and restarts the message didn't come back,
         * so I went to non-blocking and it seemed to fix the bug.
         * Need to investigate further.
         */
        audit_get_reply(fd, &rep, GET_REPLY_NONBLOCKING, 0);
    }

    return 0;
}

int audit_open()
{
    return socket(PF_NETLINK, SOCK_RAW, NETLINK_AUDIT);
}

int audit_get_reply(int fd, struct audit_reply *rep, reply_t block, int peek)
{
    ssize_t len;
    int flags;

    struct sockaddr_nl nladdr;
    socklen_t nladdrlen = sizeof(nladdr);

    if (fd < 0) {
        return -EBADF;
    }

    /* Set up the flags for recv from */
    flags = (block == GET_REPLY_NONBLOCKING) ? MSG_DONTWAIT : 0;
    flags |= peek;

    /*
     * Get the data from the netlink socket but on error we need to be carefull,
     * the interface shows that EINTR can never be returned, other errors, however,
     * can be returned.
     */
    do {
        len = recvfrom(fd, &rep->msg, sizeof(rep->msg), flags, (struct sockaddr*) &nladdr,
                &nladdrlen);

        /*
         * EAGAIN and EINTR should be re-tried until success or
         * another error manifests.
         */
        if (len < 0 && errno != EINTR) {
            if (block == GET_REPLY_NONBLOCKING && errno == EAGAIN) {
                /* If the request is non blocking and the errno is EAGAIN, just return 0 */
                return 0;
            }
            SLOGE("Error receiving from netlink socket, error: %s", strerror(errno));
            return -errno;
        }

        /* 0 or greater indicates success */
    } while (len < 0);

    if (nladdrlen != sizeof(nladdr)) {
        SLOGE("Protocol fault, error: %s", strerror(EPROTO));
        return -EPROTO;
    }

    /* Make sure the netlink message was not spoof'd */
    if (nladdr.nl_pid) {
        SLOGE("Invalid netlink pid received, expected 0 got: %d", nladdr.nl_pid);
        return -EINVAL;
    }

    return set_internal_fields(rep, len);
}

void audit_close(int fd)
{
    int rc = close(fd);
    if (rc < 0) {
        SLOGE("Attempting to close invalid fd %d, error: %s", fd, strerror(errno));
    }
    return;
}

int audit_set_logsplit(int fd, int enabled,rep_wait_t wmode)
{
    int rc;
    struct audit_reply rep;
    struct audit_logsplit_status status;

    memset(&status, 0, sizeof(status));

    if(enabled != AUDIT_LOGSPLIT_OFF &&
            enabled != AUDIT_LOGSPLIT_ON) {
        SLOGE("Invalid logsplit option of: %x\n", enabled);
        return -EINVAL;
    }

    status.enabled = enabled;

    rc = audit_send(fd, AUDIT_LOGSPLIT_SET, &status, sizeof(status));
    if (rc < 0) {
        SLOGE("Could net set splitlog for audit events, error: %s", strerror(-rc));
        return rc;
    }

    if (wmode != WAIT_NO) {
        audit_get_reply(fd, &rep, GET_REPLY_NONBLOCKING, 0);
    }

    return 0;
}
