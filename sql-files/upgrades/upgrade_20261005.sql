-- Data mods keep for themselves (src/map/mod_store.hpp): one row per stored
-- value. scope is 0 global, 1 account, 2 character; owner is the account or
-- character id, 0 for global.
CREATE TABLE IF NOT EXISTS `mod_store` (
  `mod_name` varchar(64) CHARACTER SET ascii NOT NULL,
  `scope` tinyint unsigned NOT NULL,
  `owner` int unsigned NOT NULL DEFAULT '0',
  `path` varchar(255) CHARACTER SET ascii NOT NULL,
  `kind` char(1) CHARACTER SET ascii NOT NULL DEFAULT 'i',
  `num` bigint NOT NULL DEFAULT '0',
  `str` mediumblob NULL,
  PRIMARY KEY (`mod_name`, `scope`, `owner`, `path`)
) ENGINE=MyISAM;
