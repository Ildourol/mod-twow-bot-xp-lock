-- -----------------------------------------------------------------------------
-- mod-twow-bot-xp-lock: Character Database Schema
-- Table: bot_xp_locks
-- Stores per-bot XP lock states, level caps, individual XP rates, and audit metadata.
-- -----------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS `bot_xp_locks` (
    `guid` INT UNSIGNED NOT NULL COMMENT 'Character low GUID of the bot',
    `locked` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT '1 = locked, 0 = unlocked',
    `lock_level` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 = lock at current level; >0 = cap at this level',
    `locked_at_level` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Level when lock was applied',
    `xp_rate` FLOAT NOT NULL DEFAULT 1.0 COMMENT 'Individual bot XP rate multiplier (e.g. 0.5, 1.0, 2.0)',
    `locked_by_guid` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Player low GUID who applied the lock',
    `locked_at` DATETIME NULL COMMENT 'Timestamp when lock was last modified',
    PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Turtle WoW Bot XP Lock & Rate State Registry';
