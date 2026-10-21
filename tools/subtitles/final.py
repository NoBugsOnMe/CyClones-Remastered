#!/usr/bin/env python3
"""The briefings' subtitles, proofread: the recognisers' drafts (Parakeet, Whisper small.en and
Whisper turbo compared) with names and terms as the game and its manual write them.

Each entry: start, end (seconds into the speech, from the drafts' VAD stretches) and the text;
' | ' splits an entry into cues timed by the length of each part. Written as SubRip to OUTDIR.

  final.py OUTDIR
"""
import os, sys

S = {
'225': [
    (1.400, 4.152, "Welcome to AI Central Command, HAVOC."),
    (4.152, 9.684, "I've read all of your test specifications and technical results, | and I'm looking forward to getting you into action."),
    (10.552, 15.860, "I'm Catherine Hayes, Director of Operations | and Chief Strategist for Advanced Ideas Corporation."),
    (16.664, 21.140, "I assume your beta programming has prepared you | for the level of performance that I will demand."),
    (22.008, 23.380, "Let me introduce you to my staff."),
    (24.344, 25.940, "This is Commander Trevor Shane,"),
    (26.808, 30.356, "the most decorated soldier in this or any other theater of war."),
    (31.000, 33.908, "Commander Shane is AI's Weapons and Tactics Officer."),
    (34.840, 39.668, "Major Deirdre Ducharme, Communications and Intelligence Officer, | will be your eyes and ears."),
    (40.280, 42.996, "Call me Dee Dee, if that's all right with Director Hayes."),
],
'226': [
    (0.344, 1.684, "A target droid?"),
    (3.128, 6.964, "The CyClones will chew them up and spit them back in our faces."),
    (7.896, 11.668, "I'm not willing to entrust more lives... | Not this time, Commander."),
    (12.312, 16.884, "This, staff, is HAVOC Unit, Advanced Ideas' proudest achievement."),
    (17.688, 24.376, "HAVOC was designed to infiltrate places and accomplish missions | that no squad of humans would have a prayer of."),
    (24.376, 27.892, "Your terminals are clear to review HAVOC's technical and test reports."),
    (28.504, 31.924, "Our job is to provide HAVOC mission briefing and support."),
    (32.600, 33.912, "Commander Shane,"),
    (33.912, 37.396, "commence Phase One briefing for code name Lost and Found."),
],
'202': [
    (1.304, 4.148, "This mission will separate the men from the..."),
    (5.560, 6.360, "Whatever."),
    (7.032, 11.956, "HAVOC Unit will be phase-shifted to coordinates | Alpha Bravo 100, 100."),
    (12.920, 16.408, "Once penetration of the outer walls has been achieved,"),
    (16.408, 19.892, "HAVOC Unit will conduct a search and destroy sweep of the area."),
    (20.952, 23.988, "All enemy combatants encountered will be neutralized."),
    (25.112, 26.420, "Primary mission objective:"),
    (27.064, 30.420, "find hidden entrance into lower level AI lab."),
    (31.736, 34.584, "Security doors that are located in this area"),
    (34.584, 37.364, "require the correct code keys to allow access."),
    (38.424, 41.012, "The location of the keys are unknown at this time."),
    (41.880, 44.372, "They may have already fallen into enemy hands."),
    (45.208, 47.284, "Caution must be taken in this area."),
    (48.280, 49.976, "Intelligence reports"),
    (49.976, 52.660, "that the CyClones may have a heavy weapon in the area."),
],
'203': [
    (1.240, 3.700, "Major Ducharme, commence Phase Two briefing."),
    (4.920, 10.516, "It's very important that you mark the location of all alien craft, | weapons and CyClones that are destroyed or disabled."),
    (11.128, 13.972, "This allows AI's recovery team to go in and"),
    (14.584, 16.244, "study any new opposition encounters."),
    (17.048, 23.960, "Once entrance into AI Lab No. 5 has been achieved, | HAVOC Unit will switch to combat tactics Echo Delta:"),
    (23.960, 24.788, "search and rescue."),
    (25.400, 29.336, "Neutralization of enemy units will continue as conditions dictate."),
    (29.336, 33.848, "Intel reports the security elevator located on this level has been sabotaged."),
    (33.848, 36.916, "HAVOC Unit must seek an alternative route to the lab's other sectors."),
    (38.136, 42.136, "Also located somewhere on this level is Objective One Bravo, the hostage."),
    (42.136, 44.920, "Objective One Bravo must be rescued alive."),
    (44.920, 46.296, "I repeat: alive."),
    (46.296, 52.504, "Reason: the hostage is the only known living holder of the Bio-link key, | which is used to access Hangar 18,"),
    (52.504, 53.620, "which holds an alien ship."),
    (54.392, 58.520, "A Bio-link key will only work with the full cooperation of the key holder."),
    (58.520, 63.540, "So any stress being felt by the holder of such a key is felt by the key | and will prevent the key from functioning."),
    (64.280, 67.444, "This small security measure is why the CyClones have not recaptured the ship."),
    (68.408, 73.876, "Once rescue has been achieved, HAVOC can then enter its primary objective: | One Alpha, alien ship."),
],
'204': [
    (0.536, 3.800, "Once on board, extreme caution must be used."),
    (3.800, 8.596, "Only a preliminary scan was conducted | before the ship's auto-defense computers kicked in."),
    (9.368, 12.564, "All AI equipment and personnel were destroyed,"),
    (13.272, 16.920, "and at this time no scans can penetrate the hull of the ship."),
    (16.920, 22.132, "Intel can give no further information on what may or may not | be going on inside the ship at this time."),
    (22.776, 26.008, "Once again, extreme caution is urged."),
    (26.008, 28.792, "Once the ship's internal situation has been assessed,"),
    (28.792, 31.928, "proceed with the powering up of the ship's engines."),
    (31.928, 35.476, "Once engine power is restored, proceed to the command section."),
    (36.120, 40.568, "Once the command section is located, find the data input section."),
    (40.568, 42.612, "It's the one with the round blue monitor."),
    (44.024, 47.480, "When located, place your hand on the palm print."),
    (47.480, 53.240, "An encoded nanochip placed in your hand | will input the needed data into the ship's computer."),
    (53.240, 57.504, "The ship will then autopilot itself and you back to AI control."),
],
'210': [
    (0.952, 4.788, "Code name Fail-Safe has some subtle overtones | that I want you all to be aware of."),
    (5.432, 9.492, "As you may know, the aliens' intentions toward our planet | are unknown at this time."),
    (10.232, 13.688, "Theories exist, however, that the aliens want planet Earth,"),
    (13.688, 14.868, "but not its inhabitants."),
    (15.960, 21.624, "The theory is fostered in the fact that no wholesale doomsday weapons | have been employed to this point."),
    (22.264, 28.852, "However, the implications of the CyClones' possession | of a bio-research facility on that theory are staggering."),
    (29.560, 34.488, "HAVOC will conduct penetration | of AI's Bio-Weapons Research Facility No. 1,"),
    (34.488, 38.484, "location Foxtrot Uniform 354 786,"),
    (39.096, 41.172, "no later than 1530 hours."),
    (41.848, 45.528, "Penetration will take place at the facility's main entrance,"),
    (45.528, 49.364, "location Foxtrot Uniform 135 105."),
],
'211': [
    (0.728, 5.208, "HAVOC will search the facility for the high-density data stack stored on site."),
    (5.208, 8.212, "These data stacks are constantly being updated by the facility."),
    (8.824, 14.648, "Once a data stack has been located, | its companion data relay station cannot be too far away."),
    (14.648, 18.136, "Once a data stack and its relay station have been located,"),
    (18.136, 22.484, "the HAVOC Unit will transmit that stack's information back to AI control."),
    (23.544, 26.872, "When enough information has been gathered from the data stack,"),
    (26.872, 31.480, "AI will analyze the content and determine the CyClones' course of action."),
    (31.480, 34.528, "There are known to be nine data stacks at this facility."),
],
'212': [
    (0.120, 3.156, "HAVOC Unit will then proceed to the facility's lowest level."),
    (3.768, 10.196, "Now, once the unit is at the lowest level, | it will take any action needed to start the Fail-Safe devices located..."),
    (11.736, 14.484, "There are two Fail-Safe devices located there."),
    (15.096, 19.828, "Both must be put into operation for the destruction of the facility."),
    (20.664, 25.364, "Once AI Central receives the auto-destruct signal from the facility,"),
    (26.136, 28.692, "a second security tone will be transmitted."),
    (29.688, 34.356, "This will allow operation of the emergency escape elevator | from the facility."),
    (35.256, 39.604, "No further information is available on the lab's two lower levels,"),
    (40.440, 44.920, "as the data was destroyed by a CyClone-induced computer virus"),
    (44.920, 46.772, "at the AI's data facility."),
    (48.504, 49.304, "Remember:"),
    (49.912, 53.588, "both of the facility's Fail-Safe devices must be active"),
    (54.456, 57.108, "before the extraction of HAVOC Unit will be allowed."),
],
'215': [
    (0.888, 3.480, "This is a Code Red, RDM."),
    (3.480, 11.316, "Location: SciTech Museum storage vaults | at Yankee Bravo 165 305, Z minus 30 meters."),
    (11.928, 16.436, "AI Intel reports a full-scale CyClone covert operation at Yankee Bravo."),
    (17.208, 20.632, "Primary objective of this operation is unknown at this time."),
    (20.632, 33.432, "AI data analysis shows a 96.2 percent chance | that the CyClone Covert Action Team's primary objective | is the recovery of a suspected alien artifact | being held in ISO storage at this location."),
    (33.432, 38.168, "The alien artifact was discovered at unknown location, Earth State, New Mexico,"),
    (38.168, 39.988, "year 1947."),
    (40.664, 43.892, "Artifact: CyClone origin, unsuspected until this time."),
    (44.568, 49.368, "Because of the timing and haste in which this mission | is being conducted by the CyClones,"),
    (49.368, 51.928, "the importance of the recovery of this artifact"),
    (51.928, 53.396, "cannot be stressed enough."),
    (54.552, 57.620, "HAVOC will use any means available to meet the mission requirements."),
    (58.392, 63.028, "HAVOC Unit will keep a secure comm channel open | for further orders concerning RDM."),
    (63.736, 68.436, "Any large-scale change in the CyClones' movement | will be noted and relayed to the HAVOC Unit."),
],
'230': [
    (7.096, 8.664, "Welcome back, HAVOC."),
    (8.664, 10.484, "You've been in the field too long."),
    (11.160, 14.740, "Commander Shane, commence briefing on code name mission Starfire."),
    (15.672, 17.240, "Up to now, HAVOC,"),
    (17.240, 21.204, "you've been reacting to the aliens' movements and performing covert operations."),
    (21.848, 26.520, "It is time... for you... to initiate the action."),
    (26.520, 27.348, "Bring the fight."),
    (29.656, 37.144, "HAVOC Unit will deploy with all possible speed | to SDI research and deployment facility White Star,"),
    (37.752, 43.444, "located at Lima Lima Mike Foxtrot 155 155."),
    (44.472, 48.084, "AI orbital platform Delta Niner has confirmed the presence"),
    (48.760, 50.752, "of a CyClone defense net satellite"),
],
'231': [
    (0.344, 1.812, "in orbit around Earth."),
    (3.000, 5.784, "These defense sats are the point of origin"),
    (5.784, 9.752, "for the containment fields that are projected around the CyClones"),
    (9.752, 11.188, "when they are on the attack."),
    (12.664, 17.204, "These fields are the main reason | for the CyClones' current tactical advantage."),
    (18.552, 20.884, "Once these defense fields are activated,"),
    (21.624, 26.868, "any outside reinforcement or intervention is impossible."),
    (27.672, 31.092, "This is what has been turning the tide of battle | in the enemy's favor up till now."),
    (31.832, 35.064, "These defense sats must be destroyed"),
    (35.064, 38.900, "in order for our forces to stand any chance in this conflict."),
    (39.544, 44.180, "This is a major... I say major... | turning point in this war."),
    (45.848, 47.608, "HAVOC Unit will penetrate"),
    (47.608, 53.652, "the White Star facility in an attempt to obtain the security codes | required to allow access to the missile silo areas."),
],
'217': [
    (0.600, 7.636, "Once on silo level one, HAVOC will activate each missile silo, | open each silo bay and locate the missile launch code."),
    (8.504, 11.412, "HAVOC will then proceed to silo level two and input the launch code."),
    (12.952, 19.608, "Once codes have been acknowledged by the launch computer, | an emergency evacuation elevator will be opened on this level."),
    (19.608, 23.800, "Expected enemy activity in this area is rated as very high."),
    (23.800, 27.672, "AI Central Command is counting on the completion of this mission."),
    (27.672, 28.884, "Don't let us down."),
],
'221': [
    (0.120, 6.772, "From this point, HAVOC Unit will proceed with the destruction | of the largest concentration of CyClones on the planet."),
    (7.800, 10.200, "This CyClone Hive Complex"),
    (10.200, 14.200, "showed up on combat tactical screens right after the destruction"),
    (14.200, 16.376, "of the CyClone Defense Network"),
    (16.376, 17.812, "via our SDI sats."),
    (18.776, 21.876, "With no satellite network to provide data screen cover,"),
    (22.648, 27.348, "it became clear that this is the main CyClone base of operations."),
    (28.504, 31.572, "Also, this base is their supply net center,"),
    (32.280, 35.064, "shuttling supplies from their mothership in orbit"),
    (35.064, 36.372, "to their base here on Earth."),
],
'223': [
    (0.568, 2.904, "If the opportunity exists,"),
    (2.904, 5.204, "HAVOC Unit will continue the mission"),
    (6.040, 9.332, "and move on to the CyClone mothership located in Earth orbit."),
    (10.232, 13.176, "Once on board, HAVOC will press the attack"),
    (13.176, 16.756, "and use any means at hand to bring about the destruction of the ship."),
    (17.400, 20.888, "HAVOC Unit self-preservation may be considered,"),
    (20.888, 24.564, "but destruction of the mothership complex is the overriding issue."),
    (25.848, 28.888, "First best method will be the one utilized."),
    (28.888, 30.964, "Earth Command cannot stress this enough."),
    (31.640, 32.440, "Good luck."),
    (32.888, 33.688, "Godspeed."),
],
'250': [
    (8.056, 8.856, "HAVOC Unit..."),
    (9.400, 10.200, "welcome back."),
    (11.192, 11.992, "Done well."),
    (12.760, 15.284, "You have done all that AI has asked of you."),
    (15.992, 17.236, "All that anyone could ask."),
    (18.296, 19.892, "You have served your purpose well."),
    (20.600, 23.380, "Now it is time for your final reward."),
    (24.600, 27.444, "I was there at the beginning of the HAVOC Project, and, well,"),
    (28.120, 30.260, "I believe it's only fitting that I'll be here at the end."),
    (31.576, 32.376, "You see,"),
    (32.696, 37.588, "when you are reprogrammed and modified, | you are fitted with an RCSD module,"),
    (39.064, 40.468, "of which I control."),
    (43.576, 44.376, "What's that you say?"),
    (45.880, 46.836, "RCSD?"),
    (48.920, 51.796, "Why, it stands for Remote Control Self-Destruct."),
],
}


def fmt(t):
    ms = int(round(t * 1000))
    return f'{ms // 3600000:02d}:{ms // 60000 % 60:02d}:{ms // 1000 % 60:02d},{ms % 1000:03d}'


def cues(entries):
    out = []
    for t0, t1, text in entries:
        parts = [p.strip() for p in text.split('|')]
        total = sum(len(p) for p in parts)
        t = t0
        for i, p in enumerate(parts):
            e = t1 if i == len(parts) - 1 else round(t + (t1 - t0) * len(p) / total, 3)
            out.append((t, e, p))
            t = e
    # close small gaps so the text doesn't flicker off between phrases; at least 0.8 s each
    fixed = []
    for i, (a, b, p) in enumerate(out):
        if i + 1 < len(out) and out[i + 1][0] - b < 0.6:
            b = out[i + 1][0]
        fixed.append((a, max(b, a + 0.8), p))
    return fixed


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    for id_, entries in S.items():
        cs = cues(entries)
        for (a, b, _), (c, _, _) in zip(cs, cs[1:]):
            assert b <= c + 1e-6, (id_, a, b, c)
        with open(f'{outdir}/{id_}.srt', 'w', encoding='utf-8', newline='\r\n') as f:
            for i, (a, b, p) in enumerate(cs, 1):
                f.write(f'{i}\n{fmt(a)} --> {fmt(b)}\n{p}\n\n')
        print(id_, len(cs), 'cues, longest', max(len(p) for _, _, p in cs))


if __name__ == '__main__':
    main()
