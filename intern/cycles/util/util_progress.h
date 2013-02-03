/*
 * Copyright 2011, Blender Foundation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef __UTIL_PROGRESS_H__
#define __UTIL_PROGRESS_H__

/* Progress
 *
 * Simple class to communicate progress status messages, timing information,
 * update notifications from a job running in another thread. All methods
 * except for the constructor/destructor are thread safe. */

#include "util_function.h"
#include "util_string.h"
#include "util_time.h"
#include "util_thread.h"

CCL_NAMESPACE_BEGIN

class Progress {
public:
	Progress()
	{
		tile = 0;
		sample = 0;
		start_time = time_dt();
		total_time = 0.0f;
		tile_time = 0.0f;
		status = "Initializing";
		substatus = "";
		sync_status = "";
		sync_substatus = "";
		update_cb = NULL;
		cancel = false;
		cancel_message = "";
		cancel_cb = NULL;
	}

	Progress(Progress& progress)
	{
		*this = progress;
	}

	Progress& operator=(Progress& progress)
	{
		thread_scoped_lock lock(progress.progress_mutex);

		progress.get_status(status, substatus);
		progress.get_tile(tile, total_time, tile_time);

		sample = progress.get_sample();

		return *this;
	}

	void reset()
	{
		tile = 0;
		sample = 0;
		start_time = time_dt();
		total_time = 0.0f;
		tile_time = 0.0f;
		status = "Initializing";
		substatus = "";
		sync_status = "";
		sync_substatus = "";
		cancel = false;
		cancel_message = "";
	}

	/* cancel */
	void set_cancel(const string& cancel_message_)
	{
		thread_scoped_lock lock(progress_mutex);
		cancel_message = cancel_message_;
		cancel = true;
	}

	bool get_cancel()
	{
		if(!cancel && cancel_cb)
			cancel_cb();

		return cancel;
	}

	string get_cancel_message()
	{
		thread_scoped_lock lock(progress_mutex);
		return cancel_message;
	}

	void set_cancel_callback(boost::function<void(void)> function)
	{
		cancel_cb = function;
	}

	/* tile and timing information */

	void set_start_time(double start_time_)
	{
		thread_scoped_lock lock(progress_mutex);

		start_time = start_time_;
	}

	void set_tile(int tile_, double tile_time_)
	{
		thread_scoped_lock lock(progress_mutex);

		tile = tile_;
		total_time = time_dt() - start_time;
		tile_time = tile_time_;
	}

	void get_tile(int& tile_, double& total_time_, double& tile_time_)
	{
		thread_scoped_lock lock(progress_mutex);

		tile_ = tile;
		total_time_ = (total_time > 0.0)? total_time: 0.0;
		tile_time_ = tile_time;
	}

	void reset_sample()
	{
		thread_scoped_lock lock(progress_mutex);

		sample = 0;
	}

	void increment_sample()
	{
		thread_scoped_lock lock(progress_mutex);

		sample++;
	}

	int get_sample()
	{
		return sample;
	}

	/* status messages */

	void set_status(const string& status_, const string& substatus_ = "")
	{
		{
			thread_scoped_lock lock(progress_mutex);
			status = status_;
			substatus = substatus_;
			total_time = time_dt() - start_time;
		}

		set_update();
	}

	void set_substatus(const string& substatus_)
	{
		{
			thread_scoped_lock lock(progress_mutex);
			substatus = substatus_;
			total_time = time_dt() - start_time;
		}

		set_update();
	}

	void set_sync_status(const string& status_, const string& substatus_ = "")
	{
		{
			thread_scoped_lock lock(progress_mutex);
			sync_status = status_;
			sync_substatus = substatus_;
			total_time = time_dt() - start_time;
		}

		set_update();

	}

	void set_sync_substatus(const string& substatus_)
	{
		{
			thread_scoped_lock lock(progress_mutex);
			sync_substatus = substatus_;
			total_time = time_dt() - start_time;
		}

		set_update();
	}

	void get_status(string& status_, string& substatus_)
	{
		thread_scoped_lock lock(progress_mutex);

		if(sync_status != "") {
			status_ = sync_status;
			substatus_ = sync_substatus;
		}
		else {
			status_ = status;
			substatus_ = substatus;
		}
	}

	/* callback */

	void set_update()
	{
		if(update_cb) {
			thread_scoped_lock lock(update_mutex);
			update_cb();
		}
	}

	void set_update_callback(boost::function<void(void)> function)
	{
		update_cb = function;
	}

protected:
	thread_mutex progress_mutex;
	thread_mutex update_mutex;
	boost::function<void(void)> update_cb;
	boost::function<void(void)> cancel_cb;

	int tile;    /* counter for rendered tiles */
	int sample;  /* counter of rendered samples, global for all tiles */

	double start_time;
	double total_time;
	double tile_time;

	string status;
	string substatus;

	string sync_status;
	string sync_substatus;

	volatile bool cancel;
	string cancel_message;
};

CCL_NAMESPACE_END

#endif /* __UTIL_PROGRESS_H__ */
