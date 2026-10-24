## Follow instructions in https://docs.diambra.ai/#installation
## Download the ROM and put it in ~/.diambra/roms (no need to dezip the content)
## (Optional) Create and activate a new python venv
## Install dependencies with make install or pip install -r requirements.txt

conda create -n colosseum python=3.10
conda activate colosseum
pip install -r requirements.txt

# start docker service
# ensure having 'diambra/engine' image
# proxy may cause error, fix with:
# $env:HTTP_PROXY = ""
# $env:HTTPS_PROXY = ""
# $env:ALL_PROXY = ""