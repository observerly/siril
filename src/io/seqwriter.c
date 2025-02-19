/*
 * This file is part of Siril, an astronomy image processor.
 * Copyright (C) 2005-2011 Francois Meyer (dulle at free.fr)
 * Copyright (C) 2012-2025 team free-astro (see more in AUTHORS file)
 * Reference site is https://siril.org
 *
 * Siril is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Siril is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Siril. If not, see <http://www.gnu.org/licenses/>.
 */

#include "seqwriter.h"
#include "core/siril_log.h"
#include "io/image_format_fits.h"

typedef enum {
	SEQ_OK = 0,
	SEQ_WRITE_ERROR,
	SEQ_INCOMPLETE
} seq_error;

static void init_images(struct seqwriter_data *writer, const fits *example) {
	writer->bitpix = example->bitpix;
	memcpy(writer->naxes, example->naxes, sizeof writer->naxes);
}

/* The seqwriter, or sequence writer, is a mechanism used to save images in a
 * single-file sequence using a single thread for writing. It receives image
 * writing requests from many threads, reorders the requests and writes the
 * images from start to end.
 * It is used for SER files and FITS cubes (= FITSEQ), which have specific
 * writing functions provided by the hook write_image_hook().
 *
 * Images are identified by their index. All images of a sequence must have a
 * writing request for their index, otherwise the writer will block and siril
 * will fail. Some images may be missing, which is signaled by receiving a NULL
 * image for their index. The resulting file does not have a hole, just one
 * less image as expected, although most of the time there are no expectations.
 * Because of memory usage explained in the long comment below in the file,
 * images incoming for writing should not be separated by too many indices,
 * like at most by the number of processing threads.
 */

struct _pending_write {
	fits *image;
	int index;
};

#define ABORT_TASK ((void *)0x66)

static void notify_data_freed(struct seqwriter_data *writer, int index);

int seqwriter_append_write(struct seqwriter_data *writer, fits *image, int index) {
	if (g_atomic_int_get(&writer->failed))
		return -1;
	struct _pending_write *newtask = malloc(sizeof(struct _pending_write));
	if (!newtask)
		return 1;
	newtask->image = image;
	newtask->index = index;

	g_async_queue_push(writer->writes_queue, newtask);
	return 0;
}

static void *write_worker(void *a) {
	struct seqwriter_data *writer = (struct seqwriter_data *)a;
	seq_error retval = SEQ_OK;
	int nb_frames_written = 0, current_index = 0;
	GList *next_images = NULL;

	do {
		struct _pending_write *task = NULL;
		GList *stored;
		for (stored = next_images; stored != NULL; stored = stored->next) {
			struct _pending_write *stored_task = (struct _pending_write *)stored->data;
			if (stored_task->index == current_index) {
				task = stored_task;
				next_images = g_list_delete_link(next_images, stored);
				siril_debug_print("writer: image %d obtained from waiting list\n", task->index);
				break;
			}
		}

		if (!task) {	// if not in the waiting list, try to get it from processing threads
			do {
				siril_debug_print("writer: waiting for message %d\n", current_index);
				task = g_async_queue_pop(writer->writes_queue);	// blocking
				if (task == ABORT_TASK) {
					siril_debug_print("writer: abort message\n");
					retval = SEQ_INCOMPLETE;
					break;
				}
				// allowable cases:
				// - different naxes[0] and naxes[1] if com.pref.core.allow_heterogeneous_fitseq is TRUE
				// forbidden cases:
				// - different naxes[0] and naxes[1] if com.pref.core.allow_heterogeneous_fitseq is FALSE
				// - different naxes[0] and naxes[1] for a SER
				// - different naxes[2]
				// - different bitpix
				if (writer->bitpix && task->image &&
					((writer->output_type == SEQ_FITSEQ && !com.pref.allow_heterogeneous_fitseq && memcmp(task->image->naxes, writer->naxes, 2 * sizeof writer->naxes[0])) ||
					(writer->output_type == SEQ_SER && memcmp(task->image->naxes, writer->naxes, 2 * sizeof writer->naxes[0])) ||
					task->image->naxes[2] != writer->naxes[2] ||
					task->image->bitpix != writer->bitpix)) {
					siril_log_color_message(_("Cannot add an image with different properties to an existing sequence.\n"), "red");
					retval = SEQ_WRITE_ERROR;
					break;
				}
				if (!writer->bitpix && task->image)
					init_images(writer, task->image);

				if (task->index >= 0 && task->index != current_index) {
					if (task->index < current_index) {
						siril_log_color_message(_("Invalid image index requested for write, aborting file creation\n"), "red");
						retval = SEQ_WRITE_ERROR;
						break;
					}
					siril_debug_print("writer: image %d put stored for later use\n", task->index);
					next_images = g_list_append(next_images, task);
					task = NULL;
				}
				else siril_debug_print("writer: image %d received\n", task->index);
			} while (!task);
		}
		if (!task)
			continue;
		if (retval == SEQ_INCOMPLETE)
			break;
		if (retval == SEQ_WRITE_ERROR) {
			siril_debug_print("writer: failed image %d, aborting\n", task->index);
			if (task->image)
				clearfits(task->image);
			notify_data_freed(writer, task->index);
			free(task);
			break;
		}
		if (!task->image) {
			// failed image, hole in sequence, skip it
			siril_debug_print("writer: skipping image %d\n", task->index);
			notify_data_freed(writer, task->index);
			current_index++;
			writer->frame_count--;
			free(task);
			continue;
		}

		siril_log_message(_("writer: Saving image %d, %ld layer(s), %ux%u pixels, %d bits\n"),
				task->index, task->image->naxes[2],
				task->image->rx, task->image->ry,
				task->image->type == DATA_FLOAT ? 32 : 16);

		retval = writer->write_image_hook(writer, task->image, nb_frames_written);
		clearfits(task->image);

		if (retval != SEQ_WRITE_ERROR) {
			notify_data_freed(writer, task->index);
			nb_frames_written++;
			current_index++;
		}
		free(task->image);
		free(task);
	} while (retval == SEQ_OK &&
			(writer->frame_count <= 0 || nb_frames_written < writer->frame_count));

	if (retval == SEQ_INCOMPLETE) {
		if (next_images) {
			siril_log_color_message(_("Incomplete file creation: %d file(s) remained to be written\n"), "red", g_list_length(next_images));
			if (writer->frame_count <= 0)
				writer->frame_count = nb_frames_written;
		}
		else if (writer->frame_count <= 0) {
			writer->frame_count = nb_frames_written;
			retval = SEQ_OK;
			siril_log_message(ngettext("Saved %d image in the sequence\n", "Saved %d images in the sequence\n", nb_frames_written), nb_frames_written);
		} else {
			siril_debug_print("writer: write aborted, expected %d images, got %d.\n",
					writer->frame_count, nb_frames_written);
		}
	}

	siril_debug_print("writer exits with retval %d (0: ok, 1: error, 2: incomplete)\n", retval);
	g_atomic_int_set(&writer->failed, retval);
	return GINT_TO_POINTER(retval);
}

/* frame_count can be unknown and nil or negative, otherwise, providing it will
 * give clearer output on completion of the sequence */
void start_writer(struct seqwriter_data *writer, int frame_count) {
	g_assert(writer->write_image_hook);
	g_assert(writer->sequence);
	writer->failed = 0;
	writer->bitpix = 0;
	writer->naxes[0] = 0;
	writer->frame_count = frame_count;
	if (frame_count > 0)
		siril_debug_print("writer: starting with expected frame count\n");
	writer->writes_queue = g_async_queue_new();
	writer->write_thread = g_thread_new("writer", write_worker, writer);
}

/* Stopping the writer does not unblock the threads waiting for a memory slot.
 * It's the responsability of the caller to release its slot, which will
 * unblock a thread, which must check for a cancel condition before processing,
 * and itself release the slot if not processing.
 */
int stop_writer(struct seqwriter_data *writer, gboolean aborting) {
	int retval = 0;
	if (writer->write_thread) {
		if (aborting) {
			// it aborts on next message instead of writing everything
			g_async_queue_push_front(writer->writes_queue, ABORT_TASK);
		}
		else g_async_queue_push(writer->writes_queue, ABORT_TASK);
		siril_debug_print("writer thread notified, waiting for exit...\n");
		gpointer ret = g_thread_join(writer->write_thread);
		writer->write_thread = NULL;
		g_async_queue_unref(writer->writes_queue);
		retval = GPOINTER_TO_INT(ret);
		siril_debug_print("writer thread joined (retval: %d)\n", retval);
	}
	return retval;
}

/* FITS cannot be written by several threads at the same time. We still want to
 * read and process files in parallel and save the results into a FITS
 * sequence, so instead of writing in the file from each processing thread, we
 * queue the writes and a single thread, launched manually with the
 * write_worker function, writes to the file.
 * The problem with that is memory management. In most algorithms, we limit the
 * number of threads to match memory requirements, because each thread needs
 * memory to handle the image data. With the writes queued, memory is not freed
 * when the processing ends, but the thread is ready to process more, hence
 * allocate more. We have to pause the processing until the writing thread has
 * saved a result and freed the data, otherwise siril will go out of the memory
 * limits. In case the memory is larger than what the number of thread can
 * support, the threads won't be blocked until too many images are pending
 * write.
 * The code below counts the number of active memory blocks and provides a
 * waiting function.
 */

static int nb_blocks_active, configured_max_active_blocks;
static int nb_outputs = 1;
static GCond pool_cond;
static GMutex pool_mutex;

/* here we keep the latest frame index written for each output sequence, to
 * synchronize in case of several output sequences for a single processing */
struct _outputs_struct {
	void *seq;
	int index;
};
static struct _outputs_struct *outputs;

// 0 or less means no limit
void seqwriter_set_max_active_blocks(int max) {
	siril_log_message(_("Number of images allowed in the write queue: %d (zero or less is unlimited)\n"), max);
	if (configured_max_active_blocks > 0 && max > configured_max_active_blocks) {
		int more = max - configured_max_active_blocks;
		configured_max_active_blocks = max;
		// unblock now for dynamic scaling
		for (int i = 0; i < more; i++)
			seqwriter_release_memory();
	}
	configured_max_active_blocks = max;
	nb_blocks_active = 0;
}

void seqwriter_wait_for_memory() {
	if (configured_max_active_blocks <= 0)
		return;
	siril_debug_print("entering the wait function\n");
	g_mutex_lock(&pool_mutex);
	while (nb_blocks_active >= configured_max_active_blocks) {
		siril_debug_print("  waiting for free memory slot (%d active)\n", nb_blocks_active);
		g_cond_wait(&pool_cond, &pool_mutex);
	}
	nb_blocks_active++;
	siril_debug_print("got the slot!\n");
	g_mutex_unlock(&pool_mutex);
}

static int get_output_for_seq(void *seq) {
	for (int i = 0; i < nb_outputs; i++) {
		if (!outputs[i].seq) {
			outputs[i].seq = seq;
			outputs[i].index = -1;
			return i;
		}
		if (outputs[i].seq == seq)
			return i;
	}
	siril_debug_print("### seqwriter get_output_for_seq: not found! should never happen ###\n");
	return -1;
}

static gboolean all_outputs_to_index(int index) {
	for (int i = 0; i < nb_outputs; i++) {
		if (!outputs[i].seq)
			return FALSE;
		if (outputs[i].index < index)
			return FALSE;
	}
	siril_debug_print("\tgot all outputs notified for index %d, signaling\n", index);
	return TRUE;
}

// in case of error, release the slot
void seqwriter_release_memory() {
	g_mutex_lock(&pool_mutex);
	nb_blocks_active--;
	g_cond_signal(&pool_cond);
	g_mutex_unlock(&pool_mutex);
}

// same as seqwriter_release_memory() but handles the multiple output
static void notify_data_freed(struct seqwriter_data *writer, int index) {
	g_mutex_lock(&pool_mutex);
	if (nb_outputs > 1) {
		int output_num = get_output_for_seq(writer->sequence);
		if (outputs[output_num].index + 1 != index) {
			fprintf(stderr, "inconsistent index in memory management (%d for expected %d)\n",
					outputs[output_num].index + 1, index);
		}
		outputs[output_num].index = index;
		if (!all_outputs_to_index(index)) {
			g_mutex_unlock(&pool_mutex);
			return;
		}
	}

	nb_blocks_active--;
	g_cond_signal(&pool_cond);
	g_mutex_unlock(&pool_mutex);
}

void seqwriter_set_number_of_outputs(int number_of_outputs) {
	siril_debug_print("seqwriter number of outputs: %d\n", number_of_outputs);
	nb_outputs = number_of_outputs;
	if (number_of_outputs > 1) {
		outputs = calloc(number_of_outputs, sizeof(struct _outputs_struct));
	} else {
		if (outputs)
			free(outputs);
		outputs = NULL;
	}
}
