--
-- Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
--

local sm = ARGV[1]..":"..ARGV[2]
local typ = ARGV[3]
local attr = ARGV[4]
local key = ARGV[5]
local seq = ARGV[6]
local val = ARGV[7]

local _types = KEYS[1]
local _origins = KEYS[2]
local _table = KEYS[3]
local _uves = KEYS[4]
local _values = KEYS[5]

redis.call('sadd',_types,typ)
redis.call('sadd',_origins,sm..":"..typ)
redis.call('sadd',_table,key..':'..sm..":"..typ)
redis.call('zadd',_uves,seq,key)
redis.call('hset',_values,attr,val)

return true
