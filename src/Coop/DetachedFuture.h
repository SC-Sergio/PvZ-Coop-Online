/*
 * Copyright (C) 2026 PvZ-Coop-Online contributors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef PVZ_COOP_DETACHED_FUTURE_H
#define PVZ_COOP_DETACHED_FUTURE_H

#include <exception>
#include <future>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

namespace Coop
{
	template <typename Result, typename Function>
	std::future<Result> LaunchDetachedFuture(Function&& function)
	{
		auto resultPromise = std::make_shared<std::promise<Result>>();
		std::future<Result> resultFuture = resultPromise->get_future();
		using Task = std::decay_t<Function>;
		std::thread worker([resultPromise, task = Task(std::forward<Function>(function))]() mutable
			{
				try
				{
					resultPromise->set_value(task());
				}
				catch (...)
				{
					resultPromise->set_exception(std::current_exception());
				}
			});
		worker.detach();
		return resultFuture;
	}
}

#endif
