/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>

#include <httpd.h>
#include <http_core.h>
#include <http_log.h>
#include <http_connection.h>

#include "h2_private.h"
#include "h2_bucket.h"
#include "h2_bucket_queue.h"

typedef struct h2_qdata {
    struct h2_qdata *next;
    struct h2_qdata *prev;
    void *puser;
    h2_bucket *bucket;
} h2_qdata;

apr_status_t h2_bucket_queue_init(h2_bucket_queue *q, apr_pool_t *pool)
{
    apr_status_t status = APR_SUCCESS;
    
    q->pool = pool;
    q->first = q->last = q->free = NULL;
    
    status = apr_thread_mutex_create(&q->lock, APR_THREAD_MUTEX_DEFAULT,
                                     pool);
    if (status == APR_SUCCESS) {
        status = apr_thread_cond_create(&q->has_data, pool);
    }
    
    return status;
}

void h2_bucket_queue_destroy(h2_bucket_queue *q)
{
    if (q->lock) {
        apr_thread_mutex_destroy(q->lock);
    }
    if (q->has_data) {
        apr_thread_cond_destroy(q->has_data);
    }
    
    q->last = NULL;
    while (q->first) {
        h2_qdata *qdata = q->first;
        q->first = qdata->next;
        if (qdata->bucket) {
            h2_bucket_destroy(qdata->bucket);
        }
    }
}

void h2_bucket_queue_term(h2_bucket_queue *q)
{
    apr_status_t status = apr_thread_mutex_lock(q->lock);
    if (status == APR_SUCCESS) {
        q->terminated = 1;
        apr_thread_cond_broadcast(q->has_data);
        apr_thread_mutex_unlock(q->lock);
    }
}

static void queue_unlink(h2_bucket_queue *q, h2_qdata *qdata) {
    if (q->first == qdata) {
        /* at the head */
        q->first = qdata->next;
        if (q->first) {
            q->first->prev = NULL;
        }
        else {
            /* was the last */
            q->last = NULL;
        }
    }
    else if (q->last == qdata) {
        /* at the tail */
        q->last = qdata->prev;
        if (q->last) {
            q->last->next = NULL;
        }
        else {
            /* if qdata was the last, we should not be here */
            assert(0);
        }
    }
    else {
        /* in the middle */
        qdata->next->prev = qdata->prev;
        qdata->prev->next = qdata->next;
    }
    qdata->next = qdata->prev = NULL;
}

static h2_qdata *find_first_for(h2_bucket_queue *q, void *user)
{
    for (h2_qdata *qdata = q->first; qdata; qdata = qdata->next) {
        if (!user || user == qdata->puser) {
            return qdata;
        }
    }
    return NULL;
}

apr_status_t h2_bucket_queue_user_pop_int(h2_bucket_queue *q,
                                        apr_read_type_e block,
                                        void *user,
                                        h2_bucket **pbucket,
                                        void **puser)
{
    apr_status_t status = apr_thread_mutex_lock(q->lock);
    if (status != APR_SUCCESS) {
        return status;
    }
    
    h2_qdata *qdata = find_first_for(q, user);
    while (!qdata && block == APR_BLOCK_READ && !q->terminated) {
        apr_thread_cond_wait(q->has_data, q->lock);
        qdata = find_first_for(q, user);
    }
    
    if (qdata) {
        *pbucket = qdata->bucket;
        *puser = qdata->puser;
        
        queue_unlink(q, qdata);
        memset(qdata, 0, sizeof(h2_qdata));
        qdata->next = q->free;
        q->free = qdata;
    }
    else if (block == APR_NONBLOCK_READ) {
        status = APR_EAGAIN;
    }
    else {
        status = APR_EOF;
    }
    
    apr_thread_mutex_unlock(q->lock);
    return status;
}

apr_status_t h2_bucket_queue_user_pop(h2_bucket_queue *q, apr_read_type_e block,
                                    void *user, h2_bucket **pbucket)
{
    void *dummy;
    return h2_bucket_queue_user_pop_int(q, block, user, pbucket, &dummy);
}

apr_status_t h2_bucket_queue_pop(h2_bucket_queue *q, apr_read_type_e block,
                               h2_bucket **pbucket, void **puser)
{
    return h2_bucket_queue_user_pop_int(q, block, NULL, pbucket, puser);
}

apr_status_t h2_bucket_queue_push(h2_bucket_queue *q, 
                               h2_bucket *bucket, void *puser)
{
    apr_status_t status = apr_thread_mutex_lock(q->lock);
    if (status != APR_SUCCESS) {
        return status;
    }

    if (q->terminated) {
        status = APR_EOF;
    }
    else {
        h2_qdata *qdata = q->free;
        if (qdata) {
            q->free = qdata->next;
            memset(qdata, 0, sizeof(h2_qdata));
        }
        else {
            qdata = apr_pcalloc(q->pool, sizeof(h2_qdata));
        }
        
        qdata->bucket = bucket;
        qdata->puser = puser;
        
        if (q->last) {
            q->last->next = qdata;
            qdata->prev = q->last;
            q->last = qdata;
        }
        else {
            assert(!q->first);
            q->first = q->last = qdata;
        }
        
        apr_thread_cond_broadcast(q->has_data);
    }
    apr_thread_mutex_unlock(q->lock);
    return status;
}