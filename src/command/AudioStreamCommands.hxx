// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#pragma once

#include "CommandResult.hxx"

class Client;
class Request;
class Response;

CommandResult handle_open_stream(Client &, Request, Response &);
CommandResult handle_read_stream(Client &, Request, Response &);
CommandResult handle_close_stream(Client &, Request, Response &);
