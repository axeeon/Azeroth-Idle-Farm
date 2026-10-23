CREATE TABLE IF NOT EXISTS `character_idle_farm` (
    `guid` INT UNSIGNED NOT NULL,
    `creature_entry` INT UNSIGNED NOT NULL DEFAULT 0,
    `logout_time` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`guid`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_unicode_ci;
