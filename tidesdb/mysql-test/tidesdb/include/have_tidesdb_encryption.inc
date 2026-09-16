# Bring up the keyring the engine's data-at-rest encryption takes its master key from.
#
# This server holds keys in a keyring component rather than a plugin, and a component is loaded
# from a manifest read at startup, so bringing one up means restarting the server with a manifest
# written for it.  The server's own component_keyring_file suite already does exactly that, and
# doing it any other way here would only be a second copy of it to keep in step.
#
# cleanup_tidesdb_encryption.inc undoes this, and every test that sources one sources the other.

--source include/have_component_keyring_file.inc
--source suite/component_keyring_file/inc/setup_component.inc
