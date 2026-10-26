-- Dovecot mesh: LMTP delivery affinity.
--
-- Proxies each LMTP recipient to the node that is its user's anchor, so
-- that a user's new mails arrive on one node and the mesh replicates them
-- from there. Without this, mails delivered to several nodes at once get
-- the same UIDs, which dsync then has to renumber.
--
-- Add it as the first passdb. It answers only LMTP lookups; for everything
-- else (e.g. IMAP logins) it passes the request on to the next passdb:
--
--   lmtp_proxy = yes
--   passdb mesh-route {
--     driver = lua
--     lua_file = /usr/share/dovecot/mesh-route.lua
--     lua_settings {
--       mesh_url = http://127.0.0.1:8090
--       mesh_api_key = <the mesh_api_key>
--     }
--   }
--   passdb ... { (the real passdb) }
--
-- The peers' LMTP ports come from mesh_peer_lmtp_port (default 24). If the
-- anchor is this node, or the mesh service can't answer, the user is
-- "unknown" here and the next passdb decides, which delivers locally.

local http_client
local mesh_url
local auth_header

function script_init(args)
  mesh_url = args.mesh_url or "http://127.0.0.1:8090"
  auth_header = "Bearer " .. (args.mesh_api_key or "")
  http_client = dovecot.http.client({
    request_timeout = "2s",
    request_max_attempts = 1,
  })
  return 0
end

local function url_encode(str)
  return (str:gsub("[^%w%-%._~]", function(c)
    return string.format("%%%02X", string.byte(c))
  end))
end

function auth_passdb_lookup(req)
  if req.protocol ~= "lmtp" then
    return dovecot.auth.PASSDB_RESULT_NEXT, ""
  end

  local request = http_client:request {
    url = mesh_url .. "/mesh/v1/route?user=" .. url_encode(req.user),
  }
  request:add_header("Authorization", auth_header)
  local response = request:submit()
  if response:status() ~= 200 then
    req:log_warning("mesh route lookup failed, delivering locally: " ..
                    response:status() .. " " .. response:reason())
    return dovecot.auth.PASSDB_RESULT_USER_UNKNOWN, ""
  end

  local host, port = response:payload():match("^(%S+)\t(%d+)")
  if host == nil then
    -- this node is the anchor
    return dovecot.auth.PASSDB_RESULT_USER_UNKNOWN, ""
  end
  req:log_debug("mesh: delivering on anchor " .. host .. ":" .. port)
  return dovecot.auth.PASSDB_RESULT_OK, {
    proxy = "y", host = host, port = port, nopassword = "y",
  }
end
