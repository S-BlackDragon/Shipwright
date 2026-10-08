#pragma once

// Mutation test of the test suite (docs/DECISIONES.md D-079): bugs this project already had, brought back on purpose,
// one at a time, to check that the suite still catches them. A mutant exists only in the test build (ZMP_HARNESS):
// in the demo Zmp_TestMutant() is the constant 0 and the mutated branches are dead code. It is chosen with the
// environment variable ZMP_MUTANT (the suite's --mutante) or the CVar gZmp.Test.Mutant. Several at once with "+"
// ("barras_entrar+camara_heredada"): to bring back a past bug that needed two old behaviours together.
//
// Known not to reproduce their bug any more (2026-10-04, reports/suite_rapida/HALLAZGOS.md, AH):
// barras_entrar (every active camera asks for its letterbox and HUD each frame, so the joiner's own camera asks for
// the normal screen when the cutscene ends) and cierre_casa (the state is loaded in the middle of leaving the house,
// as before D-064, and the game goes on). They are kept as the record of that.
//
//   estado_texto           a cutscene's text statics do not travel in the group state (D-065): desync on entering
//                          during a cutscene's text
//   estado_efectos         the effects' search index does not travel in the group state (D-044)
//   sabotaje_actor         one machine changes an actor outside the simulation (what the resync test does by hand)
//   robo_foco              a test window takes the PC's focus when it opens (D-063); it gives it back at once
//   sonido_sin_foco        a window without the real focus plays sound when a test says it has the focus (D-063)
//   congelon_entrar        who is playing freezes when somebody enters its group (D-070)
//   barras_entrar          black bars and no HUD after a group cutscene for who entered during it (D-059)
//   guardado_no_anfitrion  a PC that is not the host writes its save file (D-058)
//   cierre_casa            the group state is loaded in the middle of the player's own scene change (D-064: the game
//                          closed leaving Link's house into an occupied forest)
//   estado_temblor         the quakes in progress do not travel in the group state (D-081): desync on entering
//                          during a quake
//   camara_heredada        a Link that appears keeps the camera mode of the player who was there (D-080): the game
//                          reads past a camera table in rooms with a fixed background
//   cuadro_crash           a test instance that crashes keeps the game's "Crash" dialog on the screen and never
//                          ends by itself
//   escena_al_mas_cercano  during a cutscene everybody watches, actors act on the nearest player and not on the
//                          player of the scene (D-084): that player stays frozen after Gohma's death
//   portal_sobre_companero Gohma's blue warp avoids only the player of the scene, not the others (D-099): it can
//                          appear under another player, who leaves through it without stepping in
//   portal_del_fundador    a group founded in place starts with the blue warp's player at slot 0 (D-099): in that
//                          scene the warp acts only on slot 0 while it is there
//   hada_de_todos          nothing records who is reviving with a fairy (finding AB): its darkening, its black bars
//                          and its black fill are drawn on every screen once the group has a "nobody" owner
//   menu_sin_dibujo        the pause menu does not run its draw in the frames that capture the pause background
//                          again (D-085): with no frame rendered since START the game closes when the menu opens
//   puerta_al_mas_cercano  a door attends only the nearest player (D-086): a fighter standing at the bars of a
//                          combat room from inside keeps its companions out
//   tabla_sin_cerrojo      the library writes "this file does not exist" into its resource table without the lock
//                          (finding T): two threads loading at once break the table and the game closes or hangs
//   descarga_sin_cerrojo   the library unloads a resource as it did: it looks into the table without the lock and
//                          destroys the entry with the lock held (finding T)
//   archivo_fantasma       the library adds an entry to its file table, with no lock, for every file it is asked for
//                          and does not have (finding T)
//   gui_sin_iniciar        the window object's "which backend" field holds the value it had in the starts that died
//                          (finding R; the library starts it at zero now): the game closes while starting
//   vigilante_con_simbolos the hang watchdog and the state load use the system's symbol library (finding X): a game
//                          thread that takes more than 5 s inside it hangs for good, and nothing is written
//   agua_compartida        every player's camera changes the scene's lights and the sound when its eye goes under
//                          water or comes out, and a Link that appears copies the water flags of the camera that was
//                          there (finding Y): everybody sees and hears the water of one, the lights can stay on for
//                          good, and who enters the group meanwhile differs from the rest (resynchronization)
//   entrada_sin_adelanto   a player entering a group sends its inputs no earlier when they arrive late (D-094): over
//                          a connection with latency its Link can take many seconds to appear
//   ritmo_sin_dibujo       the accelerated clock of the tests paces itself on the simulation of a tick alone, without
//                          its frame (D-101): a PC busy drawing cannot follow the group, and whoever enters never
//                          catches up (test builds only: the accelerated clock exists only there)
//   captura_por_ventana    a screenshot asks the desktop for the game's window (finding AD, D-102): a window the
//                          desktop does not show is "not found", or its capture is whatever the screen has there
//   oferta_al_mas_cercano  something to lift is offered only to the nearest player (finding AK, D-109): the one
//                          facing a bomb flower or Ruto cannot pick it up while another stands nearer
//   sale_con_carga         a Link that goes away does not let go of what it carries (finding AJ, D-108): Ruto
//                          keeps a pointer to the freed Link and nobody can carry her again
//   aparicion_sin_mirar    the spawn spot of a Link that appears next to others is searched as before phase 6
//                          (finding AI, D-107): up to 315 units to a side, through walls, onto voids, lava, exits
//                          and other Links (the group split in Phantom Ganon's room with six players)
//   ruto_al_mas_cercano    Ruto by her blue warp acts on the player nearest to her, not on the one in the warp (B1,
//                          D-113): another player standing by her is teleported, turned and frozen
//   portal_sin_soltar      a player who leaves the group or the game while in the blue warp does not free it (B1,
//                          D-113): the warp, and Ruto, go on with whoever is nearest, who is taken in its place
//   salida_cuenta_doble    a player who walks out of its group keeps as its own what the group changed in that tick
//                          and the rupees still coming in (finding AL, D-115): the next group it founds adds them a
//                          second time (77 rupees and a new player saw 4)
//   estaticas_de_jefe      the statics D-116 put into the group state (bosses' cameras, Bongo Bongo's hands,
//                          Twinrova's fight and its sisters, Dark Link's AI, the hidden items' tags, the sandstorm's
//                          colours, Ganondorf's windows) and the actor pointers it made references are not loaded:
//                          this process keeps its own, as before (family 1: whoever enters a boss fight in the middle
//                          computes something else)
//   puerta_sin_esperar     the boss door opens for whoever pushes it, as before D-117: that player goes into the
//                          boss's room alone and sees the intro without the group
//   puerta_cuenta_al_de_fuera the boss door also waits for the players of the room who play somewhere else (another
//                          group): it never opens while anybody is away from the dungeon (D-117, case a)
//   agua_no_viaja          the scene's water boxes do not travel in the group state (A3, D-118): whoever is given the
//                          state keeps the water levels of its own copy of the scene (Morpha's water, the Water Temple)
//   esqueleto_ajeno        SoH's list of skeletons to patch (addresses of the skelAnimes this process initialised)
//                          survives a state load and rewrites a Link's skeleton in multiplayer (finding AN, D-118): the
//                          founder's adult Link gets the child skeleton on the machines that were child before joining
//   tambor_al_del_contexto Bongo Bongo's drum bounces "the player" of its context when any Link stands on it (finding
//                          AO, D-119): the player of the intro, in the air, is kicked up by every beat while the others
//                          stand on the drum and hangs there with the order to let it go; those on it never bounce
//   caido_retenido         a downed Link is given the "wait" order of a scripted group cutscene and keeps it when the
//                          cutscene ends (finding AQ, D-120): it stays "in a cutscene" until it leaves the room
//   intro_fhg_mas_cercano  Phantom Ganon's intro waits for the player nearest to its horse (under the middle of the
//                          arena) in the middle and then at the gate (row F2, D-123): with a partner in the middle the
//                          one at the gate never starts it
//   rayo_al_del_contexto   the shocks and bursts of Phantom Ganon's lightning hurt only while the Link nearest to them
//                          is not invincible (row F4, D-124): they go through the others while that one was just hurt
//   destello_de_todos      the white flash of the burst of Phantom Ganon's ball that hit one Link is drawn on every
//                          screen (row F4, D-124)
//   fuera_de_los_barrotes  a Link that comes into Phantom Ganon's room once its bars are up stays outside them, at the
//                          room's entrance, with no way in (row F3, D-125)
//   texto_de_uno           the text of the player a group cutscene is about is shown on that player's screen only
//                          (row F6, D-126): Phantom Ganon's last words were read by one of the six
//   intro_fd_mas_cercano   Volvagia's intro waits for the player nearest to Volvagia (under the middle of the arena)
//                          at the edge of the arena (row V3, D-128): with partners nearer to the middle it never starts
//   aliento_al_del_contexto  a flame of Volvagia's breath burns only the Link nearest to its body, and its hit makes
//                          the fire harmless for everybody for 50 ticks (row V2, D-129)
//   portal_al_alcance_fd   Volvagia's blue warp appears over the middle hole with a Link in its reach, left there by
//                          the death cutscene's arc (row V1, D-130): that Link leaves without stepping in
//   salida_al_mas_cercano  Volvagia's head bursting out of its hole pushes back and hurts only the nearest Link within
//   120
//                          (row V4, D-132): a Link behind the head is left untouched
//   calavera_tapada        in Volvagia's death cutscene its player faces wherever it faced, and the arc of the others
//                          leaves two of them between the camera and the falling skull (D-131): every screen sees a
//                          back
//   agarre_al_mas_cercano  an actor that holds a Link acts on the Link nearest to it on every tick (family T1, D-134):
//                          Morpha's tentacle drags and shakes a partner by its base and leaves the grabbed one held
//   sacudida_global        Morpha's tentacle shaking a Link starts a cutscene everybody watches (row A2, D-135): the
//                          other five stand frozen up to 150 ticks and nobody can free the grabbed one
//   intro_mo_mas_cercano   Morpha's intro waits for the player nearest to its core, far beyond the door (row A4,
//                          D-136): with partners by the door, the one on a platform never starts it
//   aviso_del_agua_abierto Navi's warning about Morpha's water stays open over the intro when it is not the intro
//                          player's (D-136): it covers the intro on every screen
//   nucleo_esconde_por_uno Morpha's core hides while the Link nearest to it is deep in the water (row A5, D-137): one
//                          partner on the bottom stops the fight for everybody above
//   portal_al_alcance_mo   Morpha's blue warp appears on the bottom of the pool with a Link in its reach (row A7,
//                          D-138): that Link leaves without stepping in

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ZMP_HARNESS
int Zmp_TestMutant(const char* name);
// robo_foco: called at the very start of main() and once per frame.
void Zmp_TestFocusMutantStart(void);
void Zmp_TestFocusMutantFrame(void);
#else
static inline int Zmp_TestMutant(const char* name) {
    (void)name;
    return 0;
}
static inline void Zmp_TestFocusMutantStart(void) {
}
static inline void Zmp_TestFocusMutantFrame(void) {
}
#endif

#ifdef __cplusplus
}
#endif
