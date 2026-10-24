from fastapi import FastAPI, Request
from fastapi.responses import StreamingResponse, JSONResponse
from pydantic import BaseModel
from typing import List, Optional, Union, Dict, Any
from transformers import AutoTokenizer, AutoModelForCausalLM, StoppingCriteria, StoppingCriteriaList
from transformers.generation import TextIteratorStreamer
import json
import time
import torch
from datetime import datetime
import asyncio
from threading import Thread
import os

# ==============================================================================
# Global configuration
# ==============================================================================
# Whether to print model input and output text on the server console
PRINT_IO_ENABLED = True  # Set to True to print, False to disable printing

# Controls the maximum number of output lines in streaming mode
MAX_OUTPUT_LINES = 5
# ==============================================================================

# Create FastAPI app
app = FastAPI()

# Add custom modeling path if needed
# import sys
# import modeling_llama

# --- Model loading ---

# Read model path from environment variable, fallback to default if not set
MODEL_PATH_ENV = os.getenv("MODEL_PATH")
if not MODEL_PATH_ENV:
    raise RuntimeError("Set MODEL_PATH to an authorized Hugging Face model ID or local model directory.")
model_name = MODEL_PATH_ENV

# Load tokenizer
tokenizer = AutoTokenizer.from_pretrained(model_name)

# Load model
# Use torch.float16 to reduce memory usage and speed up inference
# .to("cuda") moves the model to GPU
# .eval() sets the model to evaluation mode (disables dropout, etc.)
# model = modeling_llama.LlamaForCausalLM.from_pretrained(model_name, torch_dtype=torch.float16).to("cuda").eval()
model = AutoModelForCausalLM.from_pretrained(model_name, torch_dtype=torch.float16).to("cuda").eval()

# Compile model to speed up inference
# Note: torch.compile has an initial compile overhead but can make later inference faster
model = torch.compile(model)

# --- Token ID configuration ---

# Ensure eos_token_id (End-of-Sequence token ID) is properly set
# For Llama 3, <|eot_id|> is the standard EOS token
eos_token_id = tokenizer.eos_token_id
if eos_token_id is None:
    # If tokenizer.eos_token_id is not set, try to get ID for <|eot_id|>
    eos_token_id = tokenizer.convert_tokens_to_ids("<|eot_id|>")
    if eos_token_id is None:
        raise ValueError("Failed to determine EOS token ID (<|eot_id|>) for Llama 3. Please check tokenizer config.")

# Ensure pad_token_id (Padding token ID) is set
# For causal LMs, pad_token_id is usually set to eos_token_id
if tokenizer.pad_token_id is None:
    tokenizer.pad_token_id = eos_token_id
    if PRINT_IO_ENABLED:
        print(f"Set pad_token_id to eos_token_id: {tokenizer.pad_token_id}")

# Get other special token IDs for custom stopping criteria
# <|start_header_id|> and <|end_header_id|> are used in Llama 3 chat templates
start_header_id = tokenizer.convert_tokens_to_ids("<|start_header_id|>")
end_header_id = tokenizer.convert_tokens_to_ids("<|end_header_id|>")


# --- Custom stopping criteria ---

# Stop generation when any of the specified token IDs is generated
class CustomTokenStoppingCriteria(StoppingCriteria):
    def __init__(self, stop_token_ids: list[int]):
        super().__init__()
        self.stop_token_ids = stop_token_ids

    def __call__(self, input_ids: torch.LongTensor, scores: torch.FloatTensor, **kwargs) -> bool:
        # Check if the latest generated token is in the stop token list
        return input_ids[0, -1].item() in self.stop_token_ids


# Stop generation when the generated text reaches a maximum number of lines
class MaxLinesStoppingCriteria(StoppingCriteria):
    def __init__(self, max_lines: int, tokenizer, prompt_len: int):
        super().__init__()
        self.max_lines = max_lines
        self.tokenizer = tokenizer
        self.prompt_len = prompt_len

    def __call__(self, input_ids: torch.LongTensor, scores: torch.FloatTensor, **kwargs) -> bool:
        # Decode current generated text (excluding the prompt part)
        # skip_special_tokens=True to only count visible text lines
        generated_text = self.tokenizer.decode(input_ids[0][self.prompt_len:], skip_special_tokens=True)

        # Count number of lines based on '\n'
        # If text is non-empty and does not end with '\n', count one extra line
        line_count = generated_text.count("\n") + (1 if generated_text and not generated_text.endswith("\n") else 0)

        # Stop when current line count reaches the limit
        return line_count >= self.max_lines


# --- Request/response data models ---

class Message(BaseModel):
    role: str      # "user", "assistant", "system", etc.
    content: str   # message content


class ChatRequest(BaseModel):
    model: str
    messages: List[Message]
    # stream field is kept but in this implementation we always stream
    stream: Optional[bool] = True
    format: Optional[str] = None
    tools: Optional[List[Any]] = None
    options: Optional[Dict[str, Any]] = None
    keep_alive: Optional[Union[str, float]] = None


class ShowRequest(BaseModel):
    model: str


# --- API routes ---

@app.post("/api/show")
async def show(request: ShowRequest):
    """
    Show basic model info.
    Response shape is adapted to match the client's ShowResponse schema.
    """
    # Llama 3 family max context length is usually 8192 or 131072.
    # We use 131072 as a typical maximum.
    context_length = 131072

    return {
        "name": request.model,
        "model_info": {
            "name": request.model,
            "model_family": "llama",
            "model_type": "causal_lm",
            "context_length": context_length,
        },
        "modified_at": None,
        "template": None,
        "modelfile": None,
        "license": None,
        "details": None,
        "parameters": None,
        "capabilities": [],
    }


@app.post("/api/chat")
async def chat(request: Request):
    """
    Handle chat requests with streaming responses only.
    """
    # Parse request body
    body = ChatRequest(**await request.json())

    # Record request start time for duration metrics
    start_request_time = time.time()

    # --- Build model input (prompt) ---
    # Use tokenizer.apply_chat_template to format messages into Llama 3 chat format
    # add_generation_prompt=True adds the assistant-start marker at the end
    try:
        input_ids = tokenizer.apply_chat_template(
            body.messages,
            tokenize=True,
            add_generation_prompt=True,
            return_tensors="pt",
        ).to(model.device)
    except Exception as e:
        if PRINT_IO_ENABLED:
            print(f"Error: failed to apply chat template: {e}")
        return JSONResponse(status_code=400, content={"error": f"Failed to apply chat template: {e}"})

    # Print input text (including special tokens) if enabled
    if PRINT_IO_ENABLED:
        decoded_input_text = tokenizer.decode(input_ids[0], skip_special_tokens=False)
        print(f"\n--- Model input text (with special tokens) ---\n{decoded_input_text}\n---------------------------------------------\n")

    # Get max_tokens from request options, default to 64
    max_new_tokens = body.options.get("max_tokens", 64) if body.options else 64

    # --- Generation parameters ---
    # Stop tokens: EOS and possible next-turn markers
    stop_token_ids = [eos_token_id]
    if start_header_id is not None:
        stop_token_ids.append(start_header_id)
    if end_header_id is not None:
        stop_token_ids.append(end_header_id)

    # Build stopping criteria list
    stopping_criteria = StoppingCriteriaList(
        [
            CustomTokenStoppingCriteria(stop_token_ids=stop_token_ids),
            MaxLinesStoppingCriteria(
                max_lines=MAX_OUTPUT_LINES + 1, tokenizer=tokenizer, prompt_len=input_ids.shape[1]
            ),
        ]
    )

    # Prepare kwargs for model.generate
    generate_kwargs = dict(
        do_sample=body.options.get("do_sample", True) if body.options else True,
        top_p=body.options.get("top_p", 0.95) if body.options else 0.95,
        temperature=body.options.get("temperature", 0.7) if body.options else 0.7,
        max_new_tokens=max_new_tokens,
        eos_token_id=eos_token_id,
        pad_token_id=tokenizer.pad_token_id,
        stopping_criteria=stopping_criteria,
    )

    created_at = datetime.utcnow().isoformat() + "Z"
    prompt_eval_count = input_ids.shape[1]

    # === Streaming response ===
    async def stream_gen():
        # Accumulate generated text for final logging
        accumulated_generated_text = []

        # TextIteratorStreamer streams text token by token
        # skip_special_tokens=True so the client only sees readable text
        streamer = TextIteratorStreamer(tokenizer, skip_prompt=True, skip_special_tokens=True)

        generation_kwargs = {
            "input_ids": input_ids,
            "streamer": streamer,
            **generate_kwargs,
        }

        # Run model.generate in a separate thread to avoid blocking the event loop
        thread = Thread(target=model.generate, kwargs=generation_kwargs)
        thread.start()

        generated_tokens_count = 0

        # Iterate over streamer and yield partial text chunks
        for new_text in streamer:
            if new_text:  # skip empty chunks
                chunk = {
                    "model": body.model,
                    "created_at": created_at,
                    "message": {
                        "role": "assistant",
                        "content": new_text,
                        "tool_calls": [],
                    },
                    "done": False,
                }
                yield json.dumps(chunk) + "\n"
                accumulated_generated_text.append(new_text)
                generated_tokens_count += 1

        # Wait for generation thread to finish
        thread.join()

        # Print final text if enabled
        if PRINT_IO_ENABLED:
            final_output_text = "".join(accumulated_generated_text).strip()
            print(f"\n--- Total generated token chunks ---\n{generated_tokens_count}\n")
            print(f"\n--- Model output text (final) ---\n{final_output_text}\n---------------------------------------------\n")

        # Send final "done" chunk
        end_generation_time = time.time()
        total_duration_ns = int((end_generation_time - start_request_time) * 1_000_000_000)
        load_duration_ns = int(total_duration_ns * 0.1)
        prompt_eval_duration_ns = int(total_duration_ns * 0.2)
        eval_duration_ns = total_duration_ns - load_duration_ns - prompt_eval_duration_ns

        yield json.dumps(
            {
                "model": body.model,
                "created_at": created_at,
                "message": {
                    "role": "assistant",
                    "content": "",
                },
                "done": True,
                "done_reason": "stop",
                "total_duration": total_duration_ns,
                "load_duration": load_duration_ns,
                "prompt_eval_count": prompt_eval_count,
                "prompt_eval_duration": prompt_eval_duration_ns,
                "eval_count": generated_tokens_count,
                "eval_duration": eval_duration_ns,
            }
        ) + "\n"

    return StreamingResponse(stream_gen(), media_type="application/json")
