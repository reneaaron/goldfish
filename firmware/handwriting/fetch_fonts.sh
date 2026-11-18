#!/usr/bin/env bash
# Download the handwriting fonts the synthetic strips are drawn with (OFL or
# Apache 2.0, from github.com/google/fonts) into handwriting/data/fonts.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p data/fonts
cd data/fonts
for f in ofl/patrickhand/PatrickHand-Regular.ttf ofl/architectsdaughter/ArchitectsDaughter-Regular.ttf \
         ofl/indieflower/IndieFlower-Regular.ttf ofl/gochihand/GochiHand-Regular.ttf ofl/kalam/Kalam-Regular.ttf \
         ofl/shadowsintolight/ShadowsIntoLight.ttf ofl/coveredbyyourgrace/CoveredByYourGrace.ttf \
         ofl/reeniebeanie/ReenieBeanie.ttf apache/permanentmarker/PermanentMarker-Regular.ttf \
         apache/rocksalt/RockSalt-Regular.ttf ofl/gaegu/Gaegu-Regular.ttf ofl/handlee/Handlee-Regular.ttf \
         ofl/nothingyoucoulddo/NothingYouCouldDo.ttf apache/comingsoon/ComingSoon-Regular.ttf \
         ofl/sueellenfrancisco/SueEllenFrancisco-Regular.ttf ofl/waitingforthesunrise/WaitingfortheSunrise.ttf \
         ofl/zeyada/Zeyada.ttf ofl/delius/Delius-Regular.ttf; do
    curl -sfLO "https://raw.githubusercontent.com/google/fonts/main/$f" || echo "missing: $f"
done
ls *.ttf | wc -l
