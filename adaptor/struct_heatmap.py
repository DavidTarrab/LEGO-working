import os
import argparse

import torch
import torch.nn.functional as F
import numpy as np
import matplotlib.pyplot as plt

from tqdm import tqdm
from datasets import load_dataset
from transformers import AutoModelForCausalLM, AutoTokenizer


def parse_args():
    parser = argparse.ArgumentParser(
        description="Compute layer-to-layer cosine similarity of hidden states."
    )

    # Required arguments
    parser.add_argument(
        "--model_path",
        type=str,
        required=True,
        help="Path or identifier of the HuggingFace model.",
    )
    parser.add_argument(
        "--dataset_name",
        type=str,
        required=True,
        help="HuggingFace dataset name (e.g., TIGER-Lab/WebInstructSub).",
    )
    parser.add_argument(
        "--split",
        type=str,
        default="train",
        help="Dataset split to load (default: train).",
    )
    parser.add_argument(
        "--subset",
        type=str,
        default=None,
        help="Optional dataset subset/config (e.g., 'all' for MMLU).",
    )

    # Processing arguments
    parser.add_argument(
        "--batch_size",
        type=int,
        default=10,
        help="Batch size for inference.",
    )
    parser.add_argument(
        "--batch_num",
        type=int,
        default=250,
        help="Number of batches to process.",
    )

    # Output directory
    parser.add_argument(
        "--results_root",
        type=str,
        default="results",
        help="Root directory to store the output files.",
    )

    return parser.parse_args()


def main():
    args = parse_args()

    # ------------------------------
    # (1) Configuration
    # ------------------------------
    model_path = args.model_path
    dataset_name = args.dataset_name
    split = args.split
    subset = args.subset
    batch_size = args.batch_size
    batch_num = args.batch_num

    # Use model and dataset names to create result directory
    model_name = os.path.basename(os.path.normpath(model_path))
    dataset_tag = dataset_name.replace("/", "__")
    results_dir = os.path.join(args.results_root, "FullLayers", f"{model_name}__{dataset_tag}")
    os.makedirs(results_dir, exist_ok=True)

    print("===== Configuration =====")
    print(f"Model path   : {model_path}")
    print(f"Dataset      : {dataset_name}")
    if subset is not None:
        print(f"Subset       : {subset}")
    print(f"Split        : {split}")
    print(f"Batch size   : {batch_size}")
    print(f"Batch num    : {batch_num}")
    print(f"Results dir  : {results_dir}")
    print("=========================")

    # ------------------------------
    # (2) Load tokenizer and model
    # ------------------------------
    print("Loading tokenizer and model...")
    tokenizer = AutoTokenizer.from_pretrained(model_path, trust_remote_code=True)

    # Add pad token if missing
    if tokenizer.pad_token is None:
        print("Tokenizer has no pad_token. Adding [PAD]...")
        tokenizer.add_special_tokens({"pad_token": "[PAD]"})
        model = AutoModelForCausalLM.from_pretrained(
            model_path, trust_remote_code=True,
            torch_dtype=torch.float32, device_map="auto", local_files_only=True
        )
        model.resize_token_embeddings(len(tokenizer))
    else:
        model = AutoModelForCausalLM.from_pretrained(
            model_path, trust_remote_code=True,
            torch_dtype=torch.float32, device_map="auto", local_files_only=True
        )

    print("Setting model to eval mode...")
    model.eval()

    # ------------------------------
    # (3) Load dataset
    # ------------------------------
    print(f"Loading dataset {dataset_name} ({split})...")
    if subset is not None:
        dataset = load_dataset(dataset_name, subset, split=split)
    else:
        dataset = load_dataset(dataset_name, split=split)

    dataset = dataset.shuffle(seed=42)
    dataset_size = len(dataset)
    print(f"Dataset size: {dataset_size}")

    # Adjust batch_num if dataset is too small
    total_needed = batch_size * batch_num
    if total_needed > dataset_size:
        print("Requested more samples than available. Reducing batch_num...")
        batch_num = dataset_size // batch_size
        total_needed = batch_size * batch_num

    print(f"Effective batch_num: {batch_num}")
    print(f"Total samples used : {total_needed}")

    # ------------------------------
    # (4) Register forward hooks
    # ------------------------------
    layer_inputs = {}

    def create_input_hook(layer_idx):
        """Capture each layer's hidden_states before entering the layer."""
        def hook(module, inputs):
            hidden_states = inputs[0]
            layer_inputs[layer_idx] = hidden_states.detach().clone().cpu()
        return hook

    hooks = []
    for i, layer in enumerate(model.model.layers):
        hooks.append(layer.register_forward_pre_hook(create_input_hook(i)))

    # Also capture the input to the final normalization layer
    final_layer_index = len(model.model.layers)

    def norm_input_hook(module, inputs):
        layer_inputs[final_layer_index] = inputs[0].detach().clone().cpu()

    hooks.append(model.model.norm.register_forward_pre_hook(norm_input_hook))

    num_layers = len(model.model.layers) + 1
    print(f"Total transformer layers captured: {num_layers}")

    # ------------------------------
    # (5) Running statistics
    # ------------------------------
    running_sim_matrix = np.zeros((num_layers, num_layers), dtype=np.float64)
    processed_batches = 0

    print("Start processing batches...")

    # ------------------------------
    # (6) Iterate over batches
    # ------------------------------
    for batch_idx in tqdm(range(batch_num), desc="Processing Batches"):
        start_idx = batch_idx * batch_size
        end_idx = start_idx + batch_size
        data_slice = dataset.select(range(start_idx, end_idx))

        # Build input texts (assuming dataset has 'question' and 'answer')
        texts = [
            item["question"].strip() + " " + item["answer"].strip()
            for item in data_slice
        ]

        inputs = tokenizer(texts, return_tensors="pt",
                           padding=True, truncation=True).to(model.device)

        layer_inputs.clear()
        with torch.no_grad():
            _ = model(**inputs)

        # Compute cosine similarity for this batch
        sim = np.zeros((num_layers, num_layers))
        for i in range(num_layers):
            for j in range(num_layers):
                a = layer_inputs[i].view(-1, layer_inputs[i].shape[-1]).float()
                b = layer_inputs[j].view(-1, layer_inputs[j].shape[-1]).float()
                sim[i, j] = F.cosine_similarity(a, b, dim=1).mean().item()

        running_sim_matrix += sim
        processed_batches += 1

        np.save(
            os.path.join(results_dir, "layer_input_cosine_similarity_running_avg.npy"),
            running_sim_matrix / processed_batches
        )

    print("All batches processed.")

    # ------------------------------
    # (7) Remove hooks
    # ------------------------------
    for h in hooks:
        h.remove()

    # ------------------------------
    # (8) Save final matrix & heatmap
    # ------------------------------
    final_sim = running_sim_matrix / processed_batches
    np.save(os.path.join(results_dir, "layer_input_cosine_similarity_final.npy"), final_sim)

    plt.figure(figsize=(16, 14))
    plt.imshow(final_sim, cmap="Blues", interpolation="nearest", origin="lower")
    plt.colorbar(label="Cosine Similarity")
    plt.title("Layer-to-Layer Cosine Similarity (Final Avg)")
    for i in range(num_layers):
        for j in range(num_layers):
            plt.text(j, i, f"{final_sim[i,j]:.2f}", ha="center", va="center", fontsize=7)

    plt.savefig(
        os.path.join(results_dir, "layer_input_cosine_similarity_final_heatmap.png"),
        dpi=300, bbox_inches="tight"
    )
    plt.close()
    print("Done.")


if __name__ == "__main__":
    main()
