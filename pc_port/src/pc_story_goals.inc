/* Story goals for Modern's door guidance (pc_doorguide.c), in walkthrough
 * order (docs/guides/silent_hill_1_walkthrough.md). Hand-written; positions
 * and flags come from each map's MAP_EVENTS and map_points.h.
 *
 * { track, done-when flag, required flag (0 = none), expires-when flag (0 = never),
 *   map, X, Z, flags, item Harry must carry (0 = none) }
 *
 * A goal is where Harry has to be. Doors that need an item used on them
 * (keys, medallions, plates, the Five Rites) have no row: when the route is
 * blocked, the guide finds the locked door that takes an item Harry carries.
 *
 * Known gaps (no shine; the player is on their own there): the Ophiel
 * keypad ("ALERT") and the astrology keypads in Nowhere, and switching the
 * Nowhere generator off. Their flags aren't named yet. */

/* ---- main ---------------------------------------------------------------------------- */
/* Cafe */
{ TRACK_MAIN, EventFlag_M0S01_PickupKitchenKnife, 0, 0, MapIdx_MAP0_S01, Q12(5.1f), Q12(274.1f), 0, 0 },
{ TRACK_MAIN, EventFlag_M0S01_PickupMap, 0, 0, MapIdx_MAP0_S01, Q12(5.0f), Q12(267.3f), 0, 0 },
{ TRACK_MAIN, EventFlag_M0S01_AirScreamerIntroCutsceneStarted, 0, 0, MapIdx_MAP0_S01, Q12(0.8f), Q12(269.5f), 0, 0 },
{ TRACK_MAIN, EventFlag_M0S01_PickupPocketRadio, 0, 0, MapIdx_MAP0_S01, Q12(2.3f), Q12(273.9f), 0, 0 },
/* Old Silent Hill: the alley, the doghouse, the Keys for Eclipse */
{ TRACK_MAIN, 147, 0, 0, MapIdx_MAP2_S00, Q12(-186.92f), Q12(300.69f), 0, 0 },
{ TRACK_MAIN, EventFlag_M2S00_DogHouseNoteFound, 0, 0, MapIdx_MAP2_S00, Q12(-154.6f), Q12(2.0f), 0, 0 },
{ TRACK_MAIN, EventFlag_M2S00_PickupDogHouseKey, 0, 0, MapIdx_MAP2_S00, Q12(-139.06f), Q12(53.25f), 0, 0 },
{ TRACK_MAIN, EventFlag_M2S00_PickupKeyOfWoodman, 0, 0, MapIdx_MAP2_S00, Q12(-190.46f), Q12(376.6f), 0, 0 },
{ TRACK_MAIN, EventFlag_M2S00_PickupKeyOfLion, 0, 0, MapIdx_MAP2_S00, Q12(185.23f), Q12(116.08f), 0, 0 },
{ TRACK_MAIN, EventFlag_M2S00_PickupKeyOfScarecrow, 0, 0, MapIdx_MAP2_S00, Q12(99.93f), Q12(-33.1f), 0, 0 },
{ TRACK_MAIN, 158, 0, 0, MapIdx_MAP2_S00, Q12(111.9f), Q12(216.5f), 0, 0 },
/* Midwich Elementary */
{ TRACK_MAIN, EventFlag_M1S01_PickupChemical, 0, 0, MapIdx_MAP1_S01, Q12(-101.0f), Q12(100.4f), 0, 0 },
{ TRACK_MAIN, EventFlag_M1S01_PickupGoldMedallion, 0, 0, MapIdx_MAP1_S01, Q12(-59.2f), Q12(99.2f), 0, 0 },
{ TRACK_MAIN, 71, 0, 0, MapIdx_MAP1_S00, Q12(-12.2f), Q12(-67.7f), 0, 0 },
{ TRACK_MAIN, EventFlag_M1S01_PianoPuzzleSolved, 0, 0, MapIdx_MAP1_S01, Q12(-99.7f), Q12(19.9f), 0, 0 },
{ TRACK_MAIN, EventFlag_M1S01_PickupSilverMedallion, 0, 0, MapIdx_MAP1_S01, Q12(-98.8f), Q12(22.8f), 0, 0 },
{ TRACK_MAIN, 72, 0, 0, MapIdx_MAP1_S00, Q12(-13.7f), Q12(-69.1f), 0, 0 },
{ TRACK_MAIN, 73, 0, 0, MapIdx_MAP1_S00, Q12(20.6f), Q12(-17.5f), 0, 0 },
/* Nightmare school */
{ TRACK_MAIN, EventFlag_M1S02_PickupRubberBall, 0, 0, MapIdx_MAP1_S02, Q12(100.2f), Q12(100.9f), 0, 0 },
{ TRACK_MAIN, EventFlag_M1S02_PickupPictureCard, 0, 0, MapIdx_MAP1_S02, Q12(139.0f), Q12(99.3f), 0, 0 },
{ TRACK_MAIN, EventFlag_M1S02_UsedRubberBall, 0, 0, MapIdx_MAP1_S03, Q12(-10.4f), Q12(-17.6f), 0, 0 },
{ TRACK_MAIN, EventFlag_M1S02_PickupClassroomKey, 0, 0, MapIdx_MAP1_S02, Q12(-16.2f), Q12(-51.6f), 0, 0 },
{ TRACK_MAIN, EventFlag_M1S03_LockerOpened, 0, 0, MapIdx_MAP1_S03, Q12(-58.8f), Q12(18.3f), 0, 0 },
{ TRACK_MAIN, EventFlag_M1S03_PickupLibraryReserveKey, 0, 0, MapIdx_MAP1_S03, Q12(-60.32f), Q12(20.23f), 0, 0 },
{ TRACK_MAIN, 120, 0, 0, MapIdx_MAP1_S02, Q12(60.5f), Q12(-60.0f), 0, 0 },
{ TRACK_MAIN, EventFlag_M1S06_PickupKGordonKey, 0, 0, MapIdx_MAP1_S06, Q12(20.9f), Q12(-17.26f), 0, 0 },
/* Old Silent Hill, part 2 */
{ TRACK_MAIN, EventFlag_M2S00_KGordonDoorOpen, 0, 0, MapIdx_MAP2_S00, Q12(65.2f), Q12(377.5f), 0, 0 },
{ TRACK_MAIN, EventFlag_M2S01_PickupDrawbridgeKey, 0, 0, MapIdx_MAP2_S01, Q12(19.85f), Q12(23.9f), 0, 0 },
{ TRACK_MAIN, 164, 0, 0, MapIdx_MAP2_S00, Q12(-41.8f), Q12(346.0f), 0, 0 },
/* Alchemilla */
{ TRACK_MAIN, EventFlag_M3S01_PickupBasementKey, 0, 0, MapIdx_MAP3_S01, Q12(99.9f), Q12(58.2f), 0, 0 },
{ TRACK_MAIN, EventFlag_M3S01_GeneratorOn, 0, 0, MapIdx_MAP3_S01, Q12(140.5f), Q12(-20.6f), 0, 0 },
{ TRACK_MAIN, 224, 0, 0, MapIdx_MAP3_S01, Q12(57.5f), Q12(20.5f), 0, 0 },
/* Nightmare hospital */
{ TRACK_MAIN, EventFlag_M3S03_PickupPlateOfTurtle, 0, 0, MapIdx_MAP3_S03, Q12(-140.0f), Q12(-23.2f), 0, 0 },
{ TRACK_MAIN, EventFlag_M3S03_PickupPlateOfCat, 0, 0, MapIdx_MAP3_S03, Q12(-16.8f), Q12(-20.8f), 0, 0 },
{ TRACK_MAIN, EventFlag_M3S03_PickupBloodPack, 0, 0, MapIdx_MAP3_S03, Q12(-57.6f), Q12(-21.0f), 0, 0 },
{ TRACK_MAIN, EventFlag_M3S04_PickupPlateOfQueen, 0, 0, MapIdx_MAP3_S04, Q12(100.9f), Q12(59.75f), 0, 0 },
{ TRACK_MAIN, EventFlag_M3S03_PickupLighter, 0, 0, MapIdx_MAP3_S03, Q12(-102.5f), Q12(57.4f), 0, 0 },
{ TRACK_MAIN, EventFlag_M3S03_PickupPlateOfHatter, 0, 0, MapIdx_MAP3_S03, Q12(-143.35f), Q12(61.35f), 0, 0 },
{ TRACK_MAIN, 281, 0, 0, MapIdx_MAP3_S03, Q12(-18.2f), Q12(138.4f), 0, 0 },
{ TRACK_MAIN, EventFlag_M3S03_PickupBasementStoreroomKey, 0, 0, MapIdx_MAP3_S03, Q12(-98.54f), Q12(21.0f), 0, 0 },
{ TRACK_MAIN, EventFlag_M3S03_PickupDisinfectingAlcohol, 0, 0, MapIdx_MAP3_S03, Q12(-60.82f), Q12(20.0f), 0, 0 },
{ TRACK_MAIN, 256, 0, 0, MapIdx_MAP3_S05, Q12(59.6f), Q12(-61.4f), 0, 0 },
{ TRACK_MAIN, EventFlag_M3S05_CabinetPushed, 0, 0, MapIdx_MAP3_S05, Q12(57.55f), Q12(-22.8f), 0, 0 },
{ TRACK_MAIN, EventFlag_M3S05_AlcoholPouredOnVines, 0, 0, MapIdx_MAP3_S05, Q12(20.8f), Q12(-20.0f), 0, 0 },
{ TRACK_MAIN, 284, 0, 0, MapIdx_MAP3_S05, Q12(20.8f), Q12(-20.0f), 0, 0 },
{ TRACK_MAIN, EventFlag_M3S05_PickupExaminationRoomKey, 0, 0, MapIdx_MAP3_S05, Q12(20.6f), Q12(-99.1f), 0, 0 },
{ TRACK_MAIN, 258, 0, 0, MapIdx_MAP3_S04, Q12(17.0f), Q12(140.4f), 0, 0 },
{ TRACK_MAIN, EventFlag_M3S06_PickupAntiqueShopKey, 0, 0, MapIdx_MAP3_S06, Q12(95.85f), Q12(99.85f), 0, 0 },
/* Central Silent Hill: the antique shop */
{ TRACK_MAIN, EventFlag_M2S02_AntiqueShopOpen, 0, 0, MapIdx_MAP2_S02, Q12(-216.7f), Q12(108.2f), 0, 0 },
{ TRACK_MAIN, 302, 0, 0, MapIdx_MAP4_S01, Q12(178.2f), Q12(62.3f), 0, 0 },
{ TRACK_MAIN, 306, 0, 0, MapIdx_MAP4_S01, Q12(177.5f), Q12(23.6f), 0, 0 },
/* Nightmare town: Twinfeeler, Lisa, Floatstinger */
{ TRACK_MAIN, 319, 0, 0, MapIdx_MAP4_S02, Q12(16.0f), Q12(61.0f), 0, 0 },
{ TRACK_MAIN, 340, 0, 0, MapIdx_MAP4_S04, Q12(17.0f), Q12(141.0f), GOAL_NO_SPOT, 0 },
{ TRACK_MAIN, 352, 0, 0, MapIdx_MAP4_S05, 0, 0, GOAL_ANY_ROOM, 0 },
/* Sewers to the resort */
{ TRACK_MAIN, EventFlag_M5S00_PickupSewerKey, 0, 0, MapIdx_MAP5_S00, Q12(42.5f), Q12(-48.1f), 0, 0 },
{ TRACK_MAIN, 354, 0, 0, MapIdx_MAP5_S00, Q12(-92.0f), Q12(8.0f), 0, 0 },
/* Nightmare resort: boat, lighthouse, manhole */
{ TRACK_MAIN, 424, 0, 0, MapIdx_MAP6_S01, 0, 0, GOAL_ANY_ROOM, 0 },
{ TRACK_MAIN, 410, 0, 0, MapIdx_MAP6_S02, Q12(60.0f), Q12(-20.0f), GOAL_NO_SPOT, 0 },
{ TRACK_MAIN, 425, 0, 0, MapIdx_MAP6_S00, Q12(-118.0f), Q12(-58.0f), 0, 0 },
{ TRACK_MAIN, 411, 0, 0, MapIdx_MAP6_S00, Q12(-189.5f), Q12(132.5f), 0, 0 },
{ TRACK_MAIN, 433, 0, 0, MapIdx_MAP6_S03, Q12(90.15f), Q12(19.0f), 0, 0 },
{ TRACK_MAIN, 451, 0, 0, MapIdx_MAP6_S04, 0, 0, GOAL_ANY_ROOM, 0 },
/* Nowhere */
{ TRACK_MAIN, EventFlag_M7S01_PickupPliers, 0, 0, MapIdx_MAP7_S01, Q12(-18.55f), Q12(-98.98f), 0, 0 },
{ TRACK_MAIN, 525, 0, 0, MapIdx_MAP7_S01, Q12(-140.05f), Q12(-22.05f), 0, 0 },
{ TRACK_MAIN, EventFlag_M7S01_PickupAmuletOfSolomon, 0, 0, MapIdx_MAP7_S01, Q12(16.8f), Q12(-19.2f), 0, 0 },
{ TRACK_MAIN, EventFlag_M7S01_PickupStoneOfTime, 0, 0, MapIdx_MAP7_S01, Q12(-139.5f), Q12(-140.0f), 0, 0 },
{ TRACK_MAIN, EventFlag_M7S01_PickupKeyOfHagith, 0, 0, MapIdx_MAP7_S01, Q12(-16.65f), Q12(-61.46f), 0, 0 },
{ TRACK_MAIN, EventFlag_M7S01_PickupCrestOfMercury, 0, 0, MapIdx_MAP7_S01, Q12(-182.0f), Q12(-101.2f), 0, 0 },
{ TRACK_MAIN, EventFlag_M7S01_PickupRingOfContract, 0, 0, MapIdx_MAP7_S01, Q12(-178.4f), Q12(-98.8f), 0, 0 },
{ TRACK_MAIN, EventFlag_M7S01_PickupCamera, 0, 0, MapIdx_MAP7_S01, Q12(-19.6f), Q12(-141.9f), 0, 0 },
{ TRACK_MAIN, EventFlag_M7S01_PickupBirdCageKey, 0, 0, MapIdx_MAP7_S01, Q12(141.61f), Q12(-21.34f), 0, 0 },
{ TRACK_MAIN, EventFlag_M7S01_PickupKeyOfPhaleg, 0, 0, MapIdx_MAP7_S01, Q12(-180.88f), Q12(-19.78f), 0, 0 },
{ TRACK_MAIN, EventFlag_M7S02_PickupDaggerOfMelchior, 0, 0, MapIdx_MAP7_S02, Q12(21.1f), Q12(-60.75f), 0, 0 },
{ TRACK_MAIN, EventFlag_M7S02_PickupKeyOfBethor, 0, 0, MapIdx_MAP7_S02, Q12(62.2f), Q12(-61.1f), 0, 0 },
{ TRACK_MAIN, EventFlag_M7S02_PickupAnkh, 0, 0, MapIdx_MAP7_S02, Q12(21.5f), Q12(-100.8f), 0, 0 },
{ TRACK_MAIN, EventFlag_M7S02_PickupKeyOfAratron, 0, 0, MapIdx_MAP7_S02, Q12(-63.2f), Q12(-139.2f), 0, 0 },
{ TRACK_MAIN, EventFlag_M7S02_PickupDiskOfOuroboros, 0, 0, MapIdx_MAP7_S02, Q12(140.6f), Q12(-59.2f), 0, 0 },
{ TRACK_MAIN, 0, 0, 0, MapIdx_MAP7_S03, 0, 0, GOAL_ANY_ROOM, 0 },   /* below Nowhere: the end */

/* ---- optional: the Unknown Liquid (+ endings) ------------------------------------------ */
{ TRACK_LIQUID, EventFlag_M3S01_PickupPlasticBottle, 0, 224, MapIdx_MAP3_S01, Q12(22.8f), Q12(58.2f), 0, 0 },
{ TRACK_LIQUID, EventFlag_M3S01_PickupUnknownLiquid, 0, 224, MapIdx_MAP3_S01, Q12(141.6f), Q12(20.75f), 0, 0 },

/* ---- optional: Kaufmann and the vial (Good endings) ------------------------------------- */
{ TRACK_KAUFMANN, 380, 0, 402, MapIdx_MAP5_S02, Q12(139.29f), Q12(25.41f), 0, 0 },
{ TRACK_KAUFMANN, 371, 0, 402, MapIdx_MAP5_S01, Q12(52.0f), Q12(-58.0f), 0, 0 },
{ TRACK_KAUFMANN, 387, 0, 402, MapIdx_MAP5_S01, Q12(32.1f), Q12(-141.75f), 0, 0 },
{ TRACK_KAUFMANN, EventFlag_M5S03_PickupMagnet, 0, 402, MapIdx_MAP5_S03, Q12(61.1f), Q12(18.7f), 0, 0 },
{ TRACK_KAUFMANN, EventFlag_M5S01_KaufmannKeyUsed, 0, 402, MapIdx_MAP5_S01, Q12(-157.5f), Q12(-8.0f), 0, 0 },
{ TRACK_KAUFMANN, 390, 0, 402, MapIdx_MAP5_S03, Q12(104.25f), Q12(60.0f), 0, 0 },
{ TRACK_KAUFMANN, 391, 0, 402, MapIdx_MAP5_S03, Q12(101.67f), Q12(19.05f), 0, 0 },

/* ---- optional: Next Fear extras --------------------------------------------------------- */
{ TRACK_EXTRAS, EventFlag_M0S02_PickupChannelingStone, 58, 164, MapIdx_MAP0_S02, Q12(-146.0f), Q12(18.35f), 0, 0 },
{ TRACK_EXTRAS, EventFlag_M0S02_PickupGasolineTank, 62, 164, MapIdx_MAP0_S02, Q12(-177.61f), Q12(21.2f), 0, 0 },
{ TRACK_EXTRAS, EventFlag_M2S00_PickupChainsaw, 0, 164, MapIdx_MAP2_S00, Q12(-82.0f), Q12(-91.0f), 0, InvItemId_GasolineTank },
{ TRACK_EXTRAS, EventFlag_M2S00_PickupRockDrill, 0, 164, MapIdx_MAP2_S00, Q12(236.61f), Q12(345.55f), 0, InvItemId_GasolineTank },
{ TRACK_EXTRAS, EventFlag_M0S02_PickupKatana, 0, 164, MapIdx_MAP0_S02, Q12(-140.87f), Q12(62.1f), 0, 0 },
{ TRACK_EXTRAS, 456, 0, 120, MapIdx_MAP1_S03, Q12(-20.0f), Q12(-13.0f), 0, InvItemId_ChannelingStone },
{ TRACK_EXTRAS, 457, 0, 352, MapIdx_MAP4_S05, Q12(144.0f), Q12(-18.0f), 0, InvItemId_ChannelingStone },
{ TRACK_EXTRAS, 458, 0, 402, MapIdx_MAP5_S01, Q12(-160.0f), Q12(-16.0f), 0, InvItemId_ChannelingStone },
{ TRACK_EXTRAS, 459, 424, 433, MapIdx_MAP6_S01, Q12(-19.5f), Q12(59.0f), 0, InvItemId_ChannelingStone },
{ TRACK_EXTRAS, 460, 0, 433, MapIdx_MAP6_S02, Q12(60.0f), Q12(-20.0f), 0, InvItemId_ChannelingStone },
