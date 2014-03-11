#!/bin/sh

# 70-message_server_promotion.sh
# ChatServer

# Copyright (c) 2012 Apple Inc. All Rights Reserved.
#
# IMPORTANT NOTE: This file is licensed only for use on Apple-branded
# computers and is subject to the terms and conditions of the Apple Software
# License Agreement accompanying the package this file is a part of.
# You may not port this file to another platform without Apple's written consent.

#
# Copies the default jabberd and Rooms config files into /Library/Server during promotion
#

/Applications/Server.app/Contents/ServerRoot/usr/libexec/copy_message_server_config_files.sh
