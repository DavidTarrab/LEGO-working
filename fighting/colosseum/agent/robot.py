import os
import io
import random
import re
import time
from collections import defaultdict
from typing import Any, Dict, Generator, List, Literal, Optional

import numpy as np
import base64
import time
from gymnasium import spaces
from loguru import logger

from llama_index.core.schema import ImageNode
from llama_index.core.llms import ChatMessage, ChatResponse
from rich import print
from PIL import Image
from llama_index.core.base.llms.types import CompletionResponse

from .config import (
    INDEX_TO_MOVE,
    META_INSTRUCTIONS,
    META_INSTRUCTIONS_WITH_LOWER,
    MOVES,
    NB_FRAME_WAIT,
    X_SIZE,
    Y_SIZE,
)
from .observer import detect_position_from_color
from .llm import get_client, get_client_multimodal
import abc

_move_name_from_number_map = {} # Maps number to its move name (for lookup)

for i, move_name in enumerate(META_INSTRUCTIONS.keys()):
    number = i + 1  # Numbers start from 1
    _move_name_from_number_map[number] = move_name # Build the reverse map

class Robot(metaclass=abc.ABCMeta):
    observations: List[Optional[Dict[str, Any]]]  # memory
    next_steps: List[int]  # action plan
    actions: dict  # actions of the agents during a step of the game
    # actions of the agents during the previous step of the game
    previous_actions: Dict[str, List[int]]
    reward: float  # reward of the agent

    action_space: spaces.Space
    character: Optional[str] = None  # character name
    side: int  # side of the stage where playing: 0 = left, 1 = right
    current_direction: Literal["Left", "Right"]  # current direction facing
    sleepy: Optional[bool] = False  # if the robot is sleepy
    only_punch: Optional[bool] = False  # if the robot only punch
    temperature: float = 0.7  # temperature of the language model

    model: str  # model of the robot

    super_bar_own: int
    player_nb: int  # player number

    player_num: int # player num

    def __init__(
        self,
        action_space: spaces.Space,
        character: str,
        side: int,
        character_color: list,
        ennemy_color: list,
        sleepy: bool = False,
        only_punch: bool = False,
        temperature: float = 0.7,
        model: str = "mistral:mistral-large-latest",
        player_nb: int = 0,  # 0 means not specified
    ):
        self.action_space = action_space
        self.character = character
        if side == 0:
            self.current_direction = "Right"
        elif side == 1:
            self.current_direction = "Left"

        self.observations = []
        self.next_steps = []
        self.character_color = character_color
        self.ennemy_color = ennemy_color
        self.side = side
        self.sleepy = sleepy
        self.only_punch = only_punch
        self.temperature = temperature
        self.model = model
        self.previous_actions = defaultdict(list)
        self.actions = {}
        self.player_nb = player_nb

        # APM
        self.action_count = 0
        self.valid_move_count = 0
        self.invalid_move_count = 0
        self.last_action_time = 0.0 # Initialize the time of the last action
        self.act_interval1 = 60/200 # Minimum time interval between two actions
        self.act_interval2 = 60/200 # Minimum time interval between two actions
        self.apm_enabled = True
        self.first_act_time = None  # Time of the first 'act' call
        self.elapsed_since_first_act = 0.0  # Cumulative time from the first 'act' until the current time

        # Store the mappings as class members so get_moves_from_llm can access them
        self.move_name_from_number_map = _move_name_from_number_map

    def act(self) -> int:
        """
        At each game frame, we execute the first action in the list of next steps.

        An action is an integer from 0 to 18, where 0 is no action.

        See the MOVES dictionary for the mapping of actions to moves.
        """
        # assign apm
        act_interval = self.act_interval1
        if self.player_num==2:
            act_interval = self.act_interval2
        
        # Initialize the start time on the first call
        if self.first_act_time is None:
            self.first_act_time = time.time()

        # Current time
        now = time.time()

        # Update the total elapsed time from the first 'act' until now
        self.elapsed_since_first_act = now - self.first_act_time
        
        # Calculate the "ideal time" that should have passed
        ideal_elapsed_time = self.action_count * act_interval

        # If the execution is too fast, a wait (sleep) is necessary
        sleep_time = ideal_elapsed_time - self.elapsed_since_first_act
        if sleep_time > 0 and self.apm_enabled:
            time.sleep(sleep_time)
            now = time.time()
            self.elapsed_since_first_act = now - self.first_act_time  # Update the total elapsed time again

        # action count
        self.action_count += 1

        if not self.next_steps or len(self.next_steps) == 0:
            return 0  # No move

        if self.sleepy:
            return 0

        if self.only_punch:
            # Do a Hadouken
            if self.current_direction == "Right":
                self.next_steps.extend(
                    [
                        MOVES["Down"],
                        MOVES["Right+Down"],
                        MOVES["Right"],
                        MOVES["High Punch"],
                    ]
                )
            elif self.current_direction == "Left":
                self.next_steps.extend(
                    [
                        MOVES["Down"],
                        MOVES["Down+Left"],
                        MOVES["Left"],
                        MOVES["High Punch"],
                    ]
                )

        next_step = self.next_steps.pop(0)

        return next_step

    def plan(self) -> None:
        """
        The robot will plan its next steps by calling this method.

        In SF3, moves are based on combos, which are list of actions that must be executed in a sequence.

        Moves of Ken
        https://www.eventhubs.com/guides/2009/may/11/ken-street-fighter-3-third-strike-character-guide/

        Moves of Ryu
        https://www.eventhubs.com/guides/2008/may/09/ryu-street-fighter-3-third-strike-character-guide/
        """

        # If we already have a next step, we don't need to plan
        if len(self.next_steps) > 0:
            return

        # Call the LLM to get the next steps
        next_steps_from_llm = self.get_moves_from_llm()
        next_buttons_to_press = [
            button
            for combo in next_steps_from_llm
            for button in META_INSTRUCTIONS_WITH_LOWER[combo][
                self.current_direction.lower()
            ]
            # We add a wait time after each button press
            + [0] * NB_FRAME_WAIT
        ]
        self.next_steps.extend(next_buttons_to_press)
        
    def get_moves_from_llm(
        self,
    ) -> List[str]:
        """
        Get a list of moves from the language model.
        """

        invalid_moves = []
        valid_moves = []

        if os.getenv("DISABLE_LLM", "False") == "True":
            logger.debug("DISABLE_LLM is True, returning a random move by name")
            # In DISABLE_LLM mode, return a random move name directly
            return [random.choice(list(META_INSTRUCTIONS.keys())).lower()] 

        while len(valid_moves) == 0:
            llm_stream = self.call_llm()
            llm_response = ""

            for r in llm_stream:
                llm_response += r.delta

                moves_as_numbers = []
                unmatched_line_converted = False # Flag to track if an unmatched line has been converted

                for line in llm_response.splitlines():
                    stripped_line = line.strip()

                    if stripped_line == "-":
                        logger.debug(f"Skipping line containing only a hyphen: '{line}'")
                        continue

                    line_match = re.search(r"- (\d+)", stripped_line)
                    if line_match:
                        try:
                            moves_as_numbers.append(int(line_match.group(1)))
                        except ValueError:
                            logger.warning(f"Failed to convert matched number '{line_match.group(1)}' to an integer. Treating it as an invalid move (15).")
                            if not unmatched_line_converted:
                                moves_as_numbers.append(15)
                                unmatched_line_converted = True
                            else:
                                logger.debug(f"Skipping additional invalid number conversion for line: '{line}'")
                    else:
                        logger.debug(f"Line '{line.strip()}' does not match the expected move pattern.")
                        if not unmatched_line_converted:
                            logger.debug(f"Treating this line as an invalid move (15).")
                            moves_as_numbers.append(15)
                            unmatched_line_converted = True
                        else:
                            logger.debug(f"An unmatched line has already been converted to an invalid move, skipping this line: '{line}'")

                invalid_moves = []
                valid_moves = []
                
                for move_num in moves_as_numbers:
                    # Use self.move_name_from_number_map to look up the move name from its number
                    move_name = self.move_name_from_number_map.get(move_num)

                    if move_name: # If a corresponding move name was found
                        # Convert the move name to lowercase to match META_INSTRUCTIONS_WITH_LOWER keys
                        cleaned_move_name = move_name.strip().lower()

                        # Optional: Add a final check against META_INSTRUCTIONS_WITH_LOWER keys for robustness
                        if cleaned_move_name in META_INSTRUCTIONS_WITH_LOWER.keys():
                            if self.player_nb == 1:
                                print(f"[red] Player {self.player_nb} move: {cleaned_move_name}")
                            elif self.player_nb == 2:
                                print(f"[green] Player {self.player_nb} move: {cleaned_move_name}")
                            valid_moves.append(cleaned_move_name)
                        else:
                            logger.debug(f"Invalid move number: Number {move_num} maps to '{cleaned_move_name}' which is not in the known moves list.")
                            invalid_moves.append(str(move_num))
                    else:
                        logger.debug(f"Invalid move number: Number {move_num} does not map to any known move.")
                        invalid_moves.append(str(move_num)) # Log invalid numbers as strings

                # --- END OF MODIFIED SECTION ---

                if len(invalid_moves) > 1:
                    logger.warning(f"Detected multiple invalid move numbers: {invalid_moves}")

            logger.debug(f"Next move sequence: {valid_moves}")
            self.valid_move_count += len(valid_moves)
            self.invalid_move_count += len(invalid_moves)
            return valid_moves

        return [] # This line should ideally not be reached

    @abc.abstractmethod
    def call_llm(
        self,
        max_tokens: int = 50,
        top_p: float = 1.0,
    ) -> (
        Generator[ChatResponse, None, None] | Generator[CompletionResponse, None, None]
    ):
        """
        Make an API call to the language model.

        Edit this method to change the behavior of the robot!

        This should return a streaming response. The response should be a list of ChatResponse objects.
        Look into Llamaindex and make sure streaming is on.
        """
        raise NotImplementedError("call_llm method must be implemented")

    @abc.abstractmethod
    def observe(self, observation: dict, actions: dict, reward: float):
        """
        The robot will observe the environment by calling this method.

        The latest observations are at the end of the list.
        """
        # By default, we don't observe anything.
        pass


class TextRobot(Robot):
    def assign_player_num(self, num):
        self.player_num=num

    def observe(self, observation: dict, actions: dict, reward: float):
        """
        The robot will observe the environment by calling this method.

        The latest observations are at the end of the list.
        """

        # detect the position of characters and ennemy based on color
        observation["character_position"] = detect_position_from_color(
            observation, self.character_color
        )
        observation["ennemy_position"] = detect_position_from_color(
            observation, self.ennemy_color
        )

        self.observations.append(observation)
        # we delete the oldest observation if we have more than 10 observations
        if len(self.observations) > 10:
            self.observations.pop(0)

        self.reward = reward

        if actions.get("agent_0") is not None and actions.get("agent_0") != 0:
            self.previous_actions["agent_0"].append(actions["agent_0"])
        if actions.get("agent_1") is not None and actions.get("agent_1") != 0:
            self.previous_actions["agent_1"].append(actions["agent_1"])

        for key, value in actions.items():
            if len(self.previous_actions[key]) > 10:
                self.previous_actions[key].pop(0)

        # Keep track of the current direction by checking the position of the character
        # and the ennemy
        character_position = observation.get("character_position")
        ennemy_position = observation.get("ennemy_position")
        if (
            character_position is not None
            and ennemy_position is not None
            and len(character_position) == 2
            and len(ennemy_position) == 2
        ):
            if character_position[0] < ennemy_position[0]:
                self.current_direction = "Right"
            else:
                self.current_direction = "Left"

    def context_prompt(self) -> str:
        """
        Return a str of the context

        "The observation for you is Left"
        "The observation for the opponent is Left+Up"
        "The action history is Up"
        """

        # Create the position prompt
        side = self.side
        obs_own = self.observations[-1]["character_position"]
        obs_opp = self.observations[-1]["ennemy_position"]
        super_bar_own = self.observations[-1]["P" + str(side + 1)]["super_bar"][0]

        if obs_own is not None and obs_opp is not None:
            relative_position = np.array(obs_own) - np.array(obs_opp)
            normalized_relative_position = [
                relative_position[0] / X_SIZE,
                relative_position[1] / Y_SIZE,
            ]
        else:
            normalized_relative_position = [0.3, 0]

        position_prompt = ""
        if abs(normalized_relative_position[0]) > 0.15:
            # position_prompt += (
            #     "You are very far from the opponent. Move closer to the opponent."
            # )
            position_prompt += (
                "You are little far from the opponent. Use only one closer move in total to the opponent."
            )
            if normalized_relative_position[0] < 0:
                position_prompt += "Your opponent is on the right."
            else:
                position_prompt += "Your opponent is on the left."

        else:
            position_prompt += "You are close to the opponent. You should attack him."

        power_prompt = ""
        if super_bar_own >= 30:
            power_prompt = "You can now use a powerfull move. The names of the powerful moves are: Megafireball, Super attack 2."
        if super_bar_own >= 120 or super_bar_own == 0:
            power_prompt = "You can now only use very powerfull moves. The names of the very powerful moves are: Super attack 3, Super attack 4"
        # Create the last action prompt
        last_action_prompt = ""
        if len(self.previous_actions.keys()) >= 0:
            act_own_list = self.previous_actions["agent_" + str(side)]
            act_opp_list = self.previous_actions["agent_" + str(abs(1 - side))]

            if len(act_own_list) == 0:
                act_own = 0
            else:
                act_own = act_own_list[-1]
            if len(act_opp_list) == 0:
                act_opp = 0
            else:
                act_opp = act_opp_list[-1]

            str_act_own = INDEX_TO_MOVE[act_own]
            str_act_opp = INDEX_TO_MOVE[act_opp]

            last_action_prompt += f"Your last action was {str_act_own}. The opponent's last action was {str_act_opp}."

        reward = self.reward

        # Create the score prompt
        score_prompt = ""
        if reward > 0:
            score_prompt += "You are winning. Keep attacking the opponent."
        elif reward < 0:
            score_prompt += (
                "You are losing. Continue to attack the opponent but don't get hit."
            )

        # Assemble everything
        context = f"""{position_prompt}
{power_prompt}
{last_action_prompt}
Your current score is {reward}. {score_prompt}
To increase your score, move toward the opponent and attack the opponent. To prevent your score from decreasing, don't get hit by the opponent.
"""

        return context

    def call_llm(
        self,
        max_tokens: int = 50,
        top_p: float = 1.0,
    ) -> Generator[ChatResponse, None, None]:
        """
        Make an API call to the language model.

        Edit this method to change the behavior of the robot!
        """

        # Create a numbered map for the moves
        numbered_moves_map = {}
        move_list_with_numbers = []
        for i, move_name in enumerate(META_INSTRUCTIONS.keys()):
            number = i + 1  # Start numbering from 1
            numbered_moves_map[move_name] = number
            move_list_with_numbers.append(f"- {number}: {move_name}")
        # Convert the list to a string for the prompt
        move_list_for_prompt = "\n".join(move_list_with_numbers)

        # Calculate the maximum number for the range
        max_move_number = len(META_INSTRUCTIONS)

        # Get the numbers for Move Closer and Jump Closer
        move_closer_num = numbered_moves_map["Move Closer"]
        jump_closer_num = numbered_moves_map["Jump Closer"]
        
        system_prompt = f"""You are the best and most aggressive Street Fighter III 3rd strike player in the world. Your primary goal is to **aggressively attack and overwhelm your opponent**, not just to approach them.
Your character is {self.character}. Your goal is to beat the other opponent. You respond with a bullet point list of moves.
{self.context_prompt()}
The moves you can use are:
{move_list_for_prompt}
----
**For aggressive play, favor using the following moves:** Low Kick (13), Medium Kick (14), High Kick (15), Low Punch+Low Kick (16), Medium Punch+Medium Kick (17), High Punch+High Kick (18).
Reply with a bullet point list of moves using ONLY the corresponding number for each move. The number of the move should be within the range of 1 to {max_move_number}. Do not invent or include any other text or move names. The output must contain no more than 5 moves.
The format must be: `- <number of the move>`, with exactly one move per line. Strictly avoid generating 3 or more identical moves consecutively. **You can only use '{move_closer_num}' (Move Closer) or '{jump_closer_num}' (Jump Closer) a maximum of once, and only when at a long distance from the opponent. Otherwise, do not use them.**

EXAMPLES:
If you are little far and need to close distance to attack aggressively:
- 1
- 17
- 15
- 12
- 6

If you are close and focusing on aggressive attacks:
- 11
- 15
- 16
- 7
- 12
"""

        start_time = time.time()

        client = get_client(self.model, temperature=self.temperature)

        messages = [
            ChatMessage(role="system", content=system_prompt),
            ChatMessage(role="user", content="Your next moves are:"),
        ]
        resp = client.stream_chat(messages)

        logger.debug(f"LLM call to {self.model}: {system_prompt}")
        logger.debug(f"LLM call to {self.model}: {time.time() - start_time}s")

        return resp


class VisionRobot(Robot):
    def observe(self, observation: dict, actions: dict, reward: float):
        "We still use the same observation method to keep track of current characters direction"
        self.observations.append(observation)
        # we delete the oldest observation if we have more than 10 observations
        if len(self.observations) > 50:
            self.observations.pop(0)

        # detect the position of characters and ennemy based on color
        observation["character_position"] = detect_position_from_color(
            observation, self.character_color
        )
        observation["ennemy_position"] = detect_position_from_color(
            observation, self.ennemy_color
        )

        character_position = observation.get("character_position")
        ennemy_position = observation.get("ennemy_position")
        if (
            character_position is not None
            and ennemy_position is not None
            and len(character_position) == 2
            and len(ennemy_position) == 2
        ):
            if character_position[0] < ennemy_position[0]:
                self.current_direction = "Right"
            else:
                self.current_direction = "Left"

    def last_image_to_image_node(self) -> ImageNode:
        if len(self.observations) == 0:
            return ImageNode()

        rgb_array = self.observations[-1]["frame"]
        img = Image.fromarray(rgb_array)

        # Créer un buffer en mémoire
        buffer = io.BytesIO()

        # Sauvegarder l'image en format PNG dans le buffer
        img.save(buffer, format="PNG")

        # Obtenir les bytes de l'image encodée
        img_bytes = buffer.getvalue()

        # Create an ImageDocument
        return ImageNode(
            image=base64.b64encode(img_bytes).decode("utf-8"),
            image_mimetype="image/png",
        )

    def call_llm(
        self,
        max_tokens: int = 50,
        top_p: float = 1.0,
    ) -> Generator[CompletionResponse, None, None]:
        """
        Make an API call to the language model.

        Edit this method to change the behavior of the robot!
        """

        # Generate the prompts
        move_list = "- " + "\n - ".join([move for move in META_INSTRUCTIONS])
        system_prompt = f"""You are the best and most aggressive Street Fighter III 3rd strike player in the world.
Your character is {self.character}. Your goal is to beat the other opponent. You respond with a bullet point list of moves.

The current state of the game is given in the following image.

The moves you can use are:
{move_list}
----
Reply with a bullet point list of 3 moves. The format should be: `- <name of the move>` separated by a new line.
Example if the opponent is close:
- Move closer
- Medium Punch

Example if the opponent is far:
- Fireball
- Move closer"""

        start_time = time.time()

        client = get_client_multimodal(
            self.model, temperature=self.temperature
        )  # MultiModalLLM

        resp = client.stream_complete(
            prompt=system_prompt, image_documents=[self.last_image_to_image_node()]
        )

        logger.debug(f"LLM call to {self.model}: {system_prompt}")
        logger.debug(f"LLM call to {self.model}: {time.time() - start_time}s")

        return resp
