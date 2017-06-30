/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <spa/graph-scheduler3.h>

#include "pipewire/client/pipewire.h"
#include "pipewire/client/interfaces.h"

#include "pipewire/server/node.h"
#include "pipewire/server/data-loop.h"
#include "pipewire/server/main-loop.h"
#include "pipewire/server/work-queue.h"

/** \cond */
struct impl {
	struct pw_node this;

	struct pw_work_queue *work;

	bool async_init;
};

/** \endcond */

static void init_complete(struct pw_node *this);

static void update_port_ids(struct pw_node *node)
{
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	uint32_t *input_port_ids, *output_port_ids;
	uint32_t n_input_ports, n_output_ports, max_input_ports, max_output_ports;
	uint32_t i;
	struct spa_list *ports;
	int res;

	if (node->node == NULL)
		return;

	spa_node_get_n_ports(node->node,
			     &n_input_ports, &max_input_ports, &n_output_ports, &max_output_ports);

	node->info.n_input_ports = n_input_ports;
	node->info.max_input_ports = max_input_ports;
	node->info.n_output_ports = n_output_ports;
	node->info.max_output_ports = max_output_ports;

	node->input_port_map = calloc(max_input_ports, sizeof(struct pw_port *));
	node->output_port_map = calloc(max_output_ports, sizeof(struct pw_port *));

	input_port_ids = alloca(sizeof(uint32_t) * n_input_ports);
	output_port_ids = alloca(sizeof(uint32_t) * n_output_ports);

	spa_node_get_port_ids(node->node,
			      max_input_ports, input_port_ids, max_output_ports, output_port_ids);

	pw_log_debug("node %p: update_port ids %u/%u, %u/%u", node,
		     n_input_ports, max_input_ports, n_output_ports, max_output_ports);

	i = 0;
	ports = &node->input_ports;
	while (true) {
		struct pw_port *p = (ports == &node->input_ports) ? NULL :
		    SPA_CONTAINER_OF(ports, struct pw_port, link);

		if (p && i < n_input_ports && p->port_id == input_port_ids[i]) {
			node->input_port_map[p->port_id] = p;
			pw_log_debug("node %p: exiting input port %d", node, input_port_ids[i]);
			i++;
			ports = ports->next;
		} else if ((p && i < n_input_ports && input_port_ids[i] < p->port_id)
			   || i < n_input_ports) {
			struct pw_port *np;
			pw_log_debug("node %p: input port added %d", node, input_port_ids[i]);

			np = pw_port_new(node, PW_DIRECTION_INPUT, input_port_ids[i]);
			if ((res = spa_node_port_set_io(node->node, SPA_DIRECTION_INPUT,
							np->port_id, &np->io)) < 0)
				pw_log_warn("node %p: can't set input IO %d", node, res);

			spa_list_insert(ports, &np->link);
			ports = np->link.next;
			node->input_port_map[np->port_id] = np;

			if (!impl->async_init)
				pw_signal_emit(&node->port_added, node, np);
			i++;
		} else if (p) {
			node->input_port_map[p->port_id] = NULL;
			ports = ports->next;
			if (!impl->async_init)
				pw_signal_emit(&node->port_removed, node, p);
			pw_log_debug("node %p: input port removed %d", node, p->port_id);
			pw_port_destroy(p);
		} else {
			pw_log_debug("node %p: no more input ports", node);
			break;
		}
	}

	i = 0;
	ports = &node->output_ports;
	while (true) {
		struct pw_port *p = (ports == &node->output_ports) ? NULL :
		    SPA_CONTAINER_OF(ports, struct pw_port, link);

		if (p && i < n_output_ports && p->port_id == output_port_ids[i]) {
			pw_log_debug("node %p: exiting output port %d", node, output_port_ids[i]);
			i++;
			ports = ports->next;
			node->output_port_map[p->port_id] = p;
		} else if ((p && i < n_output_ports && output_port_ids[i] < p->port_id)
			   || i < n_output_ports) {
			struct pw_port *np;
			pw_log_debug("node %p: output port added %d", node, output_port_ids[i]);

			np = pw_port_new(node, PW_DIRECTION_OUTPUT, output_port_ids[i]);
			if ((res = spa_node_port_set_io(node->node, SPA_DIRECTION_OUTPUT,
							np->port_id, &np->io)) < 0)
				pw_log_warn("node %p: can't set output IO %d", node, res);

			spa_list_insert(ports, &np->link);
			ports = np->link.next;
			node->output_port_map[np->port_id] = np;

			if (!impl->async_init)
				pw_signal_emit(&node->port_added, node, np);
			i++;
		} else if (p) {
			node->output_port_map[p->port_id] = NULL;
			ports = ports->next;
			if (!impl->async_init)
				pw_signal_emit(&node->port_removed, node, p);
			pw_log_debug("node %p: output port removed %d", node, p->port_id);
			pw_port_destroy(p);
		} else {
			pw_log_debug("node %p: no more output ports", node);
			break;
		}
	}
}

static int pause_node(struct pw_node *this)
{
	int res;

	if (this->info.state <= PW_NODE_STATE_IDLE)
		return SPA_RESULT_OK;

	pw_log_debug("node %p: pause node", this);

	if ((res = spa_node_send_command(this->node,
					 &SPA_COMMAND_INIT(this->core->type.command_node.Pause))) <
	    0)
		pw_log_debug("got error %d", res);

	return res;
}

static int start_node(struct pw_node *this)
{
	int res;

	pw_log_debug("node %p: start node", this);

	if ((res = spa_node_send_command(this->node,
					 &SPA_COMMAND_INIT(this->core->type.command_node.Start))) <
	    0)
		pw_log_debug("got error %d", res);

	return res;
}

static int suspend_node(struct pw_node *this)
{
	int res = SPA_RESULT_OK;
	struct pw_port *p;

	pw_log_debug("node %p: suspend node", this);

	spa_list_for_each(p, &this->input_ports, link) {
		if ((res = pw_port_set_format(p, 0, NULL)) < 0)
			pw_log_warn("error unset format input: %d", res);
	}

	spa_list_for_each(p, &this->output_ports, link) {
		if ((res = pw_port_set_format(p, 0, NULL)) < 0)
			pw_log_warn("error unset format output: %d", res);
	}
	return res;
}

static void send_clock_update(struct pw_node *this)
{
	int res;
	struct spa_command_node_clock_update cu =
	    SPA_COMMAND_NODE_CLOCK_UPDATE_INIT(this->core->type.command_node.ClockUpdate,
					       SPA_COMMAND_NODE_CLOCK_UPDATE_TIME |
					       SPA_COMMAND_NODE_CLOCK_UPDATE_SCALE |
					       SPA_COMMAND_NODE_CLOCK_UPDATE_STATE |
					       SPA_COMMAND_NODE_CLOCK_UPDATE_LATENCY,	/* change_mask */
					       1,	/* rate */
					       0,	/* ticks */
					       0,	/* monotonic_time */
					       0,	/* offset */
					       (1 << 16) | 1,	/* scale */
					       SPA_CLOCK_STATE_RUNNING,	/* state */
					       0,	/* flags */
					       0);	/* latency */

	if (this->clock && this->live) {
		cu.body.flags.value = SPA_COMMAND_NODE_CLOCK_UPDATE_FLAG_LIVE;
		res = spa_clock_get_time(this->clock,
					 &cu.body.rate.value,
					 &cu.body.ticks.value,
					 &cu.body.monotonic_time.value);
	}

	if ((res = spa_node_send_command(this->node, (struct spa_command *) &cu)) < 0)
		pw_log_debug("got error %d", res);
}

static void on_node_done(struct spa_node *node, int seq, int res, void *user_data)
{
	struct impl *impl = user_data;
	struct pw_node *this = &impl->this;

	pw_log_debug("node %p: async complete event %d %d", this, seq, res);
	pw_work_queue_complete(impl->work, this, seq, res);
	pw_signal_emit(&this->async_complete, this, seq, res);
}

static void on_node_event(struct spa_node *node, struct spa_event *event, void *user_data)
{
	struct impl *impl = user_data;
	struct pw_node *this = &impl->this;

	if (SPA_EVENT_TYPE(event) == this->core->type.event_node.RequestClockUpdate) {
		send_clock_update(this);
	}
}

static void on_node_need_input(struct spa_node *node, void *user_data)
{
	struct impl *impl = user_data;
	struct pw_node *this = &impl->this;

	spa_graph_scheduler_pull(this->rt.sched, &this->rt.node);
	while (spa_graph_scheduler_iterate(this->rt.sched));
}

static void on_node_have_output(struct spa_node *node, void *user_data)
{
	struct impl *impl = user_data;
	struct pw_node *this = &impl->this;

	spa_graph_scheduler_push(this->rt.sched, &this->rt.node);
	while (spa_graph_scheduler_iterate(this->rt.sched));
}

static void
on_node_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id, void *user_data)
{

#if 0
	struct impl *impl = user_data;
	struct pw_node *this = &impl->this;
	struct pw_port *inport;

	pw_log_trace("node %p: reuse buffer %u", this, buffer_id);

	spa_list_for_each(inport, &this->input_ports, link) {
		struct pw_link *link;
		struct pw_port *outport;

		spa_list_for_each(link, &inport->rt.links, rt.input_link) {
			if (link->rt.input == NULL || link->rt.output == NULL)
				continue;

			outport = link->rt.output;
			outport->io.buffer_id = buffer_id;
		}
	}
#endif
}

static void node_unbind_func(void *data)
{
	struct pw_resource *resource = data;
	spa_list_remove(&resource->link);
}

static void
update_info(struct pw_node *this)
{
	this->info.id = this->global->id;
	this->info.input_formats = NULL;
	for (this->info.n_input_formats = 0;; this->info.n_input_formats++) {
		struct spa_format *fmt;

		if (spa_node_port_enum_formats(this->node,
					       SPA_DIRECTION_INPUT,
					       0, &fmt, NULL, this->info.n_input_formats) < 0)
			break;

		this->info.input_formats =
		    realloc(this->info.input_formats,
			    sizeof(struct spa_format *) * (this->info.n_input_formats + 1));
		this->info.input_formats[this->info.n_input_formats] = spa_format_copy(fmt);
	}
	this->info.output_formats = NULL;
	for (this->info.n_output_formats = 0;; this->info.n_output_formats++) {
		struct spa_format *fmt;

		if (spa_node_port_enum_formats(this->node,
					       SPA_DIRECTION_OUTPUT,
					       0, &fmt, NULL, this->info.n_output_formats) < 0)
			break;

		this->info.output_formats =
		    realloc(this->info.output_formats,
			    sizeof(struct spa_format *) * (this->info.n_output_formats + 1));
		this->info.output_formats[this->info.n_output_formats] = spa_format_copy(fmt);
	}
	this->info.props = this->properties ? &this->properties->dict : NULL;
}

static void
clear_info(struct pw_node *this)
{
	int i;

	free((char*)this->info.name);
	if (this->info.input_formats) {
		for (i = 0; i < this->info.n_input_formats; i++)
			free(this->info.input_formats[i]);
		free(this->info.input_formats);
	}

	if (this->info.output_formats) {
		for (i = 0; i < this->info.n_output_formats; i++)
			free(this->info.output_formats[i]);
		free(this->info.output_formats);
	}
	free((char*)this->info.error);

}

static int
node_bind_func(struct pw_global *global, struct pw_client *client, uint32_t version, uint32_t id)
{
	struct pw_node *this = global->object;
	struct pw_resource *resource;

	resource = pw_resource_new(client, id, global->type, 0);
	if (resource == NULL)
		goto no_mem;

	pw_resource_set_implementation(resource, global->object, PW_VERSION_NODE, NULL, node_unbind_func);

	pw_log_debug("node %p: bound to %d", this, resource->id);

	spa_list_insert(this->resource_list.prev, &resource->link);

	this->info.change_mask = ~0;
	pw_node_notify_info(resource, &this->info);

	return SPA_RESULT_OK;

      no_mem:
	pw_log_error("can't create node resource");
	pw_core_notify_error(client->core_resource,
			     client->core_resource->id, SPA_RESULT_NO_MEMORY, "no memory");
	return SPA_RESULT_NO_MEMORY;
}

static void init_complete(struct pw_node *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);

	spa_graph_node_add(this->rt.sched->graph,
			   &this->rt.node,
			   spa_graph_scheduler_default,
			   this->node);

	update_port_ids(this);
	pw_log_debug("node %p: init completed", this);
	impl->async_init = false;

	spa_list_insert(this->core->node_list.prev, &this->link);

	pw_core_add_global(this->core,
			   this->owner,
			   this->core->type.node, 0, this, node_bind_func, &this->global);

	update_info(this);

	pw_signal_emit(&this->initialized, this);

	pw_node_update_state(this, PW_NODE_STATE_SUSPENDED, NULL);
}

static const struct spa_node_callbacks node_callbacks = {
	SPA_VERSION_NODE_CALLBACKS,
	&on_node_done,
	&on_node_event,
	&on_node_need_input,
	&on_node_have_output,
	&on_node_reuse_buffer,
};

struct pw_node *pw_node_new(struct pw_core *core,
			    struct pw_resource *owner,
			    const char *name,
			    bool async,
			    struct spa_node *node,
			    struct spa_clock *clock, struct pw_properties *properties)
{
	struct impl *impl;
	struct pw_node *this;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	this->core = core;
	this->owner = owner;
	pw_log_debug("node %p: new, owner %p", this, owner);

	impl->work = pw_work_queue_new(this->core->main_loop->loop);

	this->info.name = strdup(name);
	this->properties = properties;

	this->node = node;
	this->clock = clock;
	this->data_loop = core->data_loop;

	this->rt.sched = &core->rt.sched;

	spa_list_init(&this->resource_list);

	if (spa_node_set_callbacks(this->node, &node_callbacks, impl) < 0)
		pw_log_warn("node %p: error setting callback", this);

	pw_signal_init(&this->destroy_signal);
	pw_signal_init(&this->port_added);
	pw_signal_init(&this->port_removed);
	pw_signal_init(&this->state_request);
	pw_signal_init(&this->state_changed);
	pw_signal_init(&this->free_signal);
	pw_signal_init(&this->async_complete);
	pw_signal_init(&this->initialized);

	this->info.state = PW_NODE_STATE_CREATING;

	spa_list_init(&this->input_ports);
	spa_list_init(&this->output_ports);

	if (this->node->info) {
		uint32_t i;

		if (this->properties == NULL)
			this->properties = pw_properties_new(NULL, NULL);

		if (this->properties)
			goto no_mem;

		for (i = 0; i < this->node->info->n_items; i++)
			pw_properties_set(this->properties,
					  this->node->info->items[i].key,
					  this->node->info->items[i].value);
	}

	impl->async_init = async;
	if (async) {
		pw_work_queue_add(impl->work,
				  this,
				  SPA_RESULT_RETURN_ASYNC(0), (pw_work_func_t) init_complete, NULL);
	} else {
		init_complete(this);
	}

	return this;

      no_mem:
	free((char *)this->info.name);
	free(impl);
	return NULL;
}

static int
do_node_remove(struct spa_loop *loop,
	       bool async, uint32_t seq, size_t size, void *data, void *user_data)
{
	struct pw_node *this = user_data;

	pause_node(this);

	spa_graph_node_remove(this->rt.sched->graph, &this->rt.node);

	return SPA_RESULT_OK;
}

/** Destroy a node
 * \param node a node to destroy
 *
 * Remove \a node. This will stop the transfer on the node and
 * free the resources allocated by \a node.
 *
 * \memberof pw_node
 */
void pw_node_destroy(struct pw_node *node)
{
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	struct pw_resource *resource, *tmp;
	struct pw_port *port, *tmpp;

	pw_log_debug("node %p: destroy", impl);
	pw_signal_emit(&node->destroy_signal, node);

	if (!impl->async_init) {
		spa_list_remove(&node->link);
		pw_global_destroy(node->global);
	}

	spa_list_for_each_safe(resource, tmp, &node->resource_list, link)
		pw_resource_destroy(resource);

	pw_loop_invoke(node->data_loop->loop, do_node_remove, 1, 0, NULL, true, node);

	pw_log_debug("node %p: destroy ports", node);
	spa_list_for_each_safe(port, tmpp, &node->input_ports, link)
		pw_port_destroy(port);

	spa_list_for_each_safe(port, tmpp, &node->output_ports, link)
		pw_port_destroy(port);

	pw_log_debug("node %p: free", node);
	pw_signal_emit(&node->free_signal, node);

	pw_work_queue_destroy(impl->work);

	if (node->input_port_map)
		free(node->input_port_map);
	if (node->output_port_map)
		free(node->output_port_map);

	if (node->properties)
		pw_properties_free(node->properties);
	clear_info(node);
	free(impl);

}

/**
 * pw_node_get_free_port:
 * \param node a \ref pw_node
 * \param direction a \ref pw_direction
 * \return the new port or NULL on error
 *
 * Find a new unused port in \a node with \a direction
 *
 * \memberof pw_node
 */
struct pw_port *pw_node_get_free_port(struct pw_node *node, enum pw_direction direction)
{
	uint32_t *n_ports, max_ports;
	struct spa_list *ports;
	struct pw_port *port = NULL, *p, **portmap;
	int res;
	int i;

	if (direction == PW_DIRECTION_INPUT) {
		max_ports = node->info.max_input_ports;
		n_ports = &node->info.n_input_ports;
		ports = &node->input_ports;
		portmap = node->input_port_map;
	} else {
		max_ports = node->info.max_output_ports;
		n_ports = &node->info.n_output_ports;
		ports = &node->output_ports;
		portmap = node->output_port_map;
	}

	pw_log_debug("node %p: direction %d max %u, n %u", node, direction, max_ports, *n_ports);

	/* first try to find an unlinked port */
	spa_list_for_each(p, ports, link) {
		if (spa_list_is_empty(&p->links))
			return p;
	}

	/* no port, can we create one ? */
	if (*n_ports < max_ports) {
		for (i = 0; i < max_ports; i++) {
			if (portmap[i] == NULL) {
				pw_log_debug("node %p: creating port direction %d %u", node, direction, i);

				port = pw_port_new(node, direction, i);
				if (port == NULL)
					goto no_mem;

				spa_list_insert(ports, &port->link);

				if ((res = spa_node_add_port(node->node, direction, i)) < 0) {
					pw_log_error("node %p: could not add port %d", node, i);
					pw_port_destroy(port);
					continue;
				} else {
					spa_node_port_set_io(node->node, direction, i, &port->io);
				}
				(*n_ports)++;
				portmap[i] = port;
				break;
			}
		}
	} else {
		if (!spa_list_is_empty(ports)) {
			port = spa_list_first(ports, struct pw_port, link);
			/* for output we can reuse an existing port, for input only
			 * when there is a multiplex */
			if (direction == PW_DIRECTION_INPUT && port->multiplex == NULL)
				port = NULL;
		}
	}
	return port;

      no_mem:
	pw_log_error("node %p: can't create new port", node);
	return NULL;
}

static void on_state_complete(struct pw_node *node, void *data, int res)
{
	enum pw_node_state state = SPA_PTR_TO_INT(data);
	char *error = NULL;

	pw_log_debug("node %p: state complete %d", node, res);
	if (SPA_RESULT_IS_ERROR(res)) {
		asprintf(&error, "error changing node state: %d", res);
		state = PW_NODE_STATE_ERROR;
	}
	pw_node_update_state(node, state, error);
}

static void node_activate(struct pw_node *this)
{
	struct pw_port *port;

	spa_list_for_each(port, &this->input_ports, link) {
		struct pw_link *link;
		spa_list_for_each(link, &port->links, input_link)
			pw_link_activate(link);
	}
	spa_list_for_each(port, &this->output_ports, link) {
		struct pw_link *link;
		spa_list_for_each(link, &port->links, output_link)
			pw_link_activate(link);
	}
}

/** Set th node state
 * \param node a \ref pw_node
 * \param state a \ref pw_node_state
 * \return 0 on success < 0 on error
 *
 * Set the state of \a node to \a state.
 *
 * \memberof pw_node
 */
int pw_node_set_state(struct pw_node *node, enum pw_node_state state)
{
	int res = SPA_RESULT_OK;
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);

	pw_signal_emit(&node->state_request, node, state);

	pw_log_debug("node %p: set state %s", node, pw_node_state_as_string(state));

	switch (state) {
	case PW_NODE_STATE_CREATING:
		return SPA_RESULT_ERROR;

	case PW_NODE_STATE_SUSPENDED:
		res = suspend_node(node);
		break;

	case PW_NODE_STATE_IDLE:
		res = pause_node(node);
		break;

	case PW_NODE_STATE_RUNNING:
		node_activate(node);
		send_clock_update(node);
		res = start_node(node);
		break;

	case PW_NODE_STATE_ERROR:
		break;
	}
	if (SPA_RESULT_IS_ERROR(res))
		return res;

	pw_work_queue_add(impl->work,
			  node, res, (pw_work_func_t) on_state_complete, SPA_INT_TO_PTR(state));

	return res;
}

/** Update the node state
 * \param node a \ref pw_node
 * \param state a \ref pw_node_state
 * \param error error when \a state is \ref PW_NODE_STATE_ERROR
 *
 * Update the state of a node. This method is used from inside \a node
 * itself.
 *
 * \memberof pw_node
 */
void pw_node_update_state(struct pw_node *node, enum pw_node_state state, char *error)
{
	enum pw_node_state old;

	old = node->info.state;
	if (old != state) {
		struct pw_resource *resource;

		pw_log_debug("node %p: update state from %s -> %s", node,
			     pw_node_state_as_string(old), pw_node_state_as_string(state));

		if (node->info.error)
			free((char*)node->info.error);
		node->info.error = error;
		node->info.state = state;

		pw_signal_emit(&node->state_changed, node, old, state);

		node->info.change_mask = 1 << 5;
		spa_list_for_each(resource, &node->resource_list, link) {
			pw_node_notify_info(resource, &node->info);
		}
	}
}
