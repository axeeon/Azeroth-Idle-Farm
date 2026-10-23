ALTER TABLE `character_idle_farm`
    ADD COLUMN `farm_zone` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `creature_entry`;
