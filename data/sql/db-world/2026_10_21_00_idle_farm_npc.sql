DELETE FROM `creature_template` WHERE `entry` = 9850001;
INSERT INTO `creature_template`
    (`entry`, `name`, `subname`, `minlevel`, `maxlevel`, `faction`, `npcflag`, `unit_class`, `flags_extra`, `ScriptName`)
VALUES
    (9850001, 'Idle Farming Master', 'Farm While You Are Away', 80, 80, 35, 1, 1, 2, 'npc_idle_farm_master');

DELETE FROM `creature_template_model` WHERE `CreatureID` = 9850001;
INSERT INTO `creature_template_model`
    (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
VALUES
    (9850001, 0, 1859, 1, 1, 0);
