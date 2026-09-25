# Citations

Every research paper, standard, dataset and external algorithm source that
OpenOSV uses or has evaluated, with where it appears in the project.

Each entry gives the authors, the title, the venue and year, and a link (a
DOI where one exists). The line under it says how OpenOSV relates to it:

* **Implemented**: OpenOSV's own code implements the method or standard.
* **Used**: OpenOSV runs the authors' own released model (only SEA-RAFT,
  in the optional neural flow backend).
* **Reference**: consulted, compared against, or the source of a documented
  fact or design choice; not implemented as such.
* **Evaluated**: measured or surveyed in the research documents and scripts;
  not part of the shipped library or plug-ins.

Licences are given for code or weights that OpenOSV uses, and for code or
weights that a survey recorded. Bibliographic details were checked against
the publishers, arXiv, CVF, OpenReview or the official repositories on
2026-09-23.

Where the project learned something from DJI's own software rather than from
a publication, that is recorded in `docs/LEGAL.md`, not here.

## Contents

1. Stitching, seams and blending
2. Photometric alignment and filtering
3. Dual-fisheye and 360 stitching
4. Learned image and video stitching
5. Optical flow
6. Monocular depth
7. Harmonisation and segmentation
8. Inpainting and generative models
9. Lens flare and veiling glare
10. Lens model, geometry and numerical methods
11. Colour standards and colour science
12. File formats and codecs
13. Open-source projects used as references
14. Patents reviewed

## 1. Stitching, seams and blending

- Shai Avidan, Ariel Shamir. **Seam carving for content-aware image
  resizing.** *ACM Transactions on Graphics* 26(3) (SIGGRAPH 2007), 2007.
  <https://doi.org/10.1145/1276377.1276390>
  - Implemented: the dynamic-programming optimal seam behind the carved
    stitch seam (`include/osv/render/SeamCarve.h`, `src/osv/render/SeamCarve.cpp`).
- Vivek Kwatra, Arno Schödl, Irfan Essa et al. **Graphcut textures: image
  and video synthesis using graph cuts.** *ACM Transactions on Graphics*
  22(3) (SIGGRAPH 2003), 2003. <https://doi.org/10.1145/882262.882264>
  - Reference: optimal seams used for stitching (`SeamCarve.h`).
- Federico Perazzi, Alexander Sorkine-Hornung, Henning Zimmer et al.
  **Panoramic Video from Unstructured Camera Arrays.** *Computer Graphics
  Forum* 34(2) (Eurographics 2015), 2015. <https://doi.org/10.1111/cgf.12541>
  - Reference: seams for video stitching (`SeamCarve.h`); its Poisson
    extrapolation of the warp is the approach `ParallaxWarp.h` deliberately
    replaces with a cheaper decay ring.
- Peter J. Burt, Edward H. Adelson. **A multiresolution spline with
  application to image mosaics.** *ACM Transactions on Graphics* 2(4), 1983.
  <https://doi.org/10.1145/245.247>
  - Evaluated: a two-band blend along the carved seam, measured and dropped
    (`SeamCarve.h`); multi-band blending in the photometry survey
    (`docs/research/NEURAL_STITCHING.md` section 3).
- Matthew Brown, David G. Lowe. **Automatic Panoramic Image Stitching using
  Invariant Features.** *International Journal of Computer Vision* 74(1),
  2007. <https://doi.org/10.1007/s11263-006-0002-3>
  - Reference: the gain-compensation solve used to describe DJI's
    compensator (`NEURAL_STITCHING.md` section 2).
- Patrick Pérez, Michel Gangnet, Andrew Blake. **Poisson image editing.**
  *ACM Transactions on Graphics* 22(3) (SIGGRAPH 2003), 2003.
  <https://doi.org/10.1145/882262.882269>
  - Evaluated: the gradient-domain (Poisson) seam blend in
    `research/neural/blend_experiments.py`.
- Zeev Farbman, Raanan Fattal, Dani Lischinski. **Convolution pyramids.**
  *ACM Transactions on Graphics* 30(6) (SIGGRAPH Asia 2011), 2011.
  <https://doi.org/10.1145/2070781.2024209>
  - Evaluated: fast membrane interpolation, in the photometry survey
    (`NEURAL_STITCHING.md` section 3).
- Zeev Farbman, Gil Hoffer, Yaron Lipman et al. **Coordinates for instant
  image cloning.** *ACM Transactions on Graphics* 28(3) (SIGGRAPH 2009),
  2009. <https://doi.org/10.1145/1531326.1531373>
  - Evaluated: seam membrane, in the photometry survey
    (`NEURAL_STITCHING.md` section 3).
- Robert Anderson, David Gallup, Jonathan T. Barron et al. **Jump: Virtual
  Reality Video.** *ACM Transactions on Graphics* 35(6) (SIGGRAPH Asia 2016),
  2016. <https://doi.org/10.1145/2980179.2980257>
  - Reference: warping both views halfway (`include/osv/render/FlowWarp.h`),
    anisotropic disparity search ranges (`ParallaxWarp.h`), and the
    observation that changing ghosting is worse than constant ghosting
    (`NEURAL_STITCHING.md` section 5).

## 2. Photometric alignment and filtering

- Dan B. Goldman, Jiun-Hung Chen. **Vignette and exposure calibration and
  compensation.** *Tenth IEEE International Conference on Computer Vision
  (ICCV)*, 2005. <https://doi.org/10.1109/ICCV.2005.249>
- Dan B. Goldman. **Vignette and Exposure Calibration and Compensation.**
  *IEEE Transactions on Pattern Analysis and Machine Intelligence* 32(12),
  2010. <https://doi.org/10.1109/TPAMI.2010.55>
  - Reference (both): what the lens overlap can and cannot tell about
    vignetting; a calibration route for common-mode vignetting
    (`NEURAL_STITCHING.md` sections 1.5, 3 and 7).
- Paul Bergmann, Rui Wang, Daniel Cremers. **Online Photometric Calibration
  of Auto Exposure Video for Realtime Visual Odometry and SLAM.** *IEEE
  Robotics and Automation Letters* 3(2), 2018.
  <https://doi.org/10.1109/LRA.2017.2777002>
  - Evaluated: temporal vignetting and exposure calibration, in the survey
    (`NEURAL_STITCHING.md` sections 1.5 and 3). Code: BSD-3-Clause.
- Kaiming He, Jian Sun, Xiaoou Tang. **Guided Image Filtering.** *ECCV
  2010*, 2010. <https://doi.org/10.1007/978-3-642-15549-9_1>
  - Reference: the local model behind DJI's glare pass, as described in
    `NEURAL_STITCHING.md` section 2. Evaluated: a guided-filter-style colour
    transfer in `research/neural/blend_experiments.py`.
- Zhou Wang, Alan C. Bovik, Hamid R. Sheikh, Eero P. Simoncelli. **Image
  quality assessment: from error visibility to structural similarity.**
  *IEEE Transactions on Image Processing* 13(4), 2004.
  <https://doi.org/10.1109/TIP.2003.819861>
  - Reference: the SSIM gate in DJI's glare pass, as described in
    `NEURAL_STITCHING.md` section 2.
- William M. Wells. **Efficient Synthesis of Gaussian Filters by Cascaded
  Uniform Filters.** *IEEE Transactions on Pattern Analysis and Machine
  Intelligence* PAMI-8(2), 1986. <https://doi.org/10.1109/TPAMI.1986.4767776>
  - Implemented: Gaussian smoothing as cascaded box filters in the flare
    analysis (`src/osv/render/Flare.cpp`).

## 3. Dual-fisheye and 360 stitching

- Tuan Ho, Madhukar Budagavi. **Dual-fisheye lens stitching for 360-degree
  imaging.** *IEEE ICASSP*, 2017. <https://doi.org/10.1109/ICASSP.2017.7952541>
- Tuan Ho, Ioannis D. Schizas, K. R. Rao, Madhukar Budagavi. **360-degree
  video stitching for dual-fisheye lens cameras based on rigid moving least
  squares.** *IEEE ICIP*, 2017. <https://doi.org/10.1109/ICIP.2017.8296241>
  - Evaluated (both): flat-field fall-off compensation and rigid MLS
    alignment, in the survey (`NEURAL_STITCHING.md` sections 1.5, 3 and 4).
    Reference code by the first author, drNoob13/fisheyeStitcher, is MIT.
- I-Chan Lo, Kuang-Tsu Shih, Homer H. Chen. **Efficient and Accurate
  Stitching for 360° Dual-Fisheye Images and Videos.** *IEEE Transactions on
  Image Processing* 31, 2022. <https://doi.org/10.1109/TIP.2021.3130531>
  - Evaluated: photometric compensation and adaptive seams, in the survey
    (`NEURAL_STITCHING.md` sections 3 and 4).
- Zhanjie Jin, Anming Dong, Jiguo Yu et al. **Dual-Fisheye Image Stitching
  via Unsupervised Deep Learning.** *MultiMedia Modeling (MMM 2024)*, 2024.
  <https://doi.org/10.1007/978-3-031-53311-2_21>
- Mufeng Zhu, Yang Sui, Bo Yuan, Yao Liu. **Learning-based Homography Matrix
  Optimization for Dual-fisheye Video Stitching.** *Workshop on Emerging
  Multimedia Systems (EMS '23)*, 2023. <https://doi.org/10.1145/3609395.3610600>
- Sooho Kim, Soyeon Hong, Kyungsoo Park et al. **OmniStitch: Depth-Aware
  Stitching Framework for Omnidirectional Vision with Multiple Cameras.**
  *ACM Multimedia 2024*, 2024. <https://doi.org/10.1145/3664647.3681208>
- Dun Dai, Ze Lu, Cheng He et al. **From Multi-Fisheye Sensing to Panoramic
  Perception: A Parallax-Aware Onboard Platform for Ultra-Low-Altitude
  UAVs.** arXiv preprint, 2026. <https://arxiv.org/abs/2609.02319>
  - Evaluated (these four): surveyed (`NEURAL_STITCHING.md` section 4). No
    usable code was found for any of them.

## 4. Learned image and video stitching

All evaluated in the learned-stitching survey (`NEURAL_STITCHING.md`
section 4); none is used by OpenOSV.

- Lang Nie, Chunyu Lin, Kang Liao et al. **Unsupervised Deep Image
  Stitching: Reconstructing Stitched Features to Images.** *IEEE Transactions
  on Image Processing* 30, 2021. <https://doi.org/10.1109/TIP.2021.3092828>
  (UDIS; no code licence.)
- Lang Nie, Chunyu Lin, Kang Liao et al. **Parallax-Tolerant Unsupervised
  Deep Image Stitching.** *ICCV 2023*, 2023.
  <https://arxiv.org/abs/2302.08207> (UDIS++; code Apache-2.0.)
- Lang Nie, Chunyu Lin, Kang Liao et al. **Eliminating Warping Shakes for
  Unsupervised Online Video Stitching.** *ECCV 2024*, 2024.
  <https://doi.org/10.1007/978-3-031-73235-5_22> (StabStitch; code
  Apache-2.0.)
- Lang Nie, Chunyu Lin, Kang Liao et al. **StabStitch++: Unsupervised Online
  Video Stitching With Spatiotemporal Bidirectional Warps.** *IEEE
  Transactions on Pattern Analysis and Machine Intelligence* 47(9), 2025.
  <https://doi.org/10.1109/TPAMI.2025.3568829> (Code Apache-2.0. Its online
  warp-trajectory smoothing is noted as an idea worth re-implementing.)
- Yuan Mei, Lang Nie, Kang Liao et al. **UniStitch: Unifying Semantic and
  Geometric Features for Image Stitching.** *ECCV 2026*, 2026.
  <https://arxiv.org/abs/2603.10568> (Code Apache-2.0.)
- Lang Nie, Yuan Mei, Kang Liao et al. **Robust Image Stitching With Optimal
  Plane.** *IEEE Transactions on Visualization and Computer Graphics* 32(7),
  2026. <https://doi.org/10.1109/TVCG.2026.3663425> (RopStitch; no code
  licence.)
- Zhiying Jiang, Ruhao Yan, Zengxi Zhang et al. **Depth-Supervised Fusion
  Network for Seamless-Free Image Stitching.** *NeurIPS 2025*, 2025.
  <https://openreview.net/forum?id=zQqDqfja4Y> (DSFN; no code licence.)
- Minsu Kim, Jaewon Lee, Byeonghun Lee et al. **Implicit Neural Image
  Stitching With Enhanced and Blended Feature Reconstruction.** *WACV 2024*,
  2024. <https://arxiv.org/abs/2309.01409> (NIS; no code licence.)
- Minsu Kim, Yongjun Lee, Woo Kyoung Han, Kyong Hwan Jin. **Learning
  Residual Elastic Warps for Image Stitching Under Dirichlet Boundary
  Condition.** *WACV 2024*, 2024. <https://arxiv.org/abs/2309.01406>
  (REwarp; no code licence.)
- Senmao Cheng, Fan Yang, Zhi Chen et al. **Deep Seam Prediction for Image
  Stitching Based on Selection Consistency Loss.** arXiv preprint, 2023.
  <https://arxiv.org/abs/2302.05027> (DSeam.)
- Ziqi Xie, Weidong Zhao, Xianhui Liu et al. **Reconstructing the Image
  Stitching Pipeline: Integrating Fusion and Rectangling into a Unified
  Inpainting Model.** *NeurIPS 2024*, 2024.
  <https://openreview.net/forum?id=ZViYPzh9Wq> (SRStitcher; code MIT; runs
  Stable Diffusion 2 inpainting.)
- Ziqi Xie, Xiao Lai, Weidong Zhao et al. **Modification Takes Courage:
  Seamless Image Stitching via Reference-Driven Inpainting.** arXiv preprint,
  2024. <https://arxiv.org/abs/2411.10309> (RDIStitcher; code Apache-2.0; a
  LoRA on Stable Diffusion 2 inpainting.)
- Wei-Sheng Lai, Jia-Bin Huang, Oliver Wang et al. **Learning Blind Video
  Temporal Consistency.** *ECCV 2018*, 2018.
  <https://doi.org/10.1007/978-3-030-01267-0_11>
  - Evaluated: post-hoc temporal consistency; its authors' own statement of
    what it cannot fix is quoted in `NEURAL_STITCHING.md` section 5.

## 5. Optical flow

"The AI-stitching study" below is the WP-AISTITCH measurement of newer flow
models on the lens-overlap band (`research/aistitch/flow_eval.py`,
`docs/research/AI_STITCHING.md`). It loads the models' checkpoints through
ptlflow for research only; none of them is shipped.

- Till Kroeger, Radu Timofte, Dengxin Dai, Luc Van Gool. **Fast Optical Flow
  Using Dense Inverse Search.** *ECCV 2016*, 2016.
  <https://doi.org/10.1007/978-3-319-46493-0_29>
  - Implemented: the classical flow backend on the CPU and in CUDA
    (`include/osv/render/DisFlow.h`, `src/osv/render/DisFlow.cpp`,
    `src/osv/render/cuda/CudaDisKernel.cu`), written from the paper; most
    of its default parameters are DJI's (`docs/LEGAL.md` section 5.2). The
    authors' reference code (GPL-3.0) is not used.
- Thomas Brox, Andrés Bruhn, Nils Papenberg, Joachim Weickert. **High
  Accuracy Optical Flow Estimation Based on a Theory for Warping.** *ECCV
  2004*, 2004. <https://doi.org/10.1007/978-3-540-24673-2_3>
  - Reference: the variational refinement DIS borrows, deliberately not
    implemented (`DisFlow.h`).
- Yihan Wang, Lahav Lipson, Jia Deng. **SEA-RAFT: Simple, Efficient, Accurate
  RAFT for Optical Flow.** *ECCV 2024*, 2024.
  <https://doi.org/10.1007/978-3-031-72667-5_3>
  - Used: the optional "Neural" flow backend runs the authors' small model,
    exported to ONNX by `scripts/fetch_flow_model.py` and loaded by
    `src/osv/render/FlowBackendOnnx.cpp`. Code and weights BSD-3-Clause
    (`NOTICE`). Also measured against DIS (`NEURAL_STITCHING.md` section 1.6).
- Zachary Teed, Jia Deng. **RAFT: Recurrent All-Pairs Field Transforms for
  Optical Flow.** *ECCV 2020*, 2020.
  <https://doi.org/10.1007/978-3-030-58536-5_24>
  - The architecture SEA-RAFT builds on. Evaluated in the AI-stitching
    study. Code BSD-3-Clause.
- Shihao Jiang, Dylan Campbell, Yao Lu et al. **Learning to Estimate Hidden
  Motions with Global Motion Aggregation.** *ICCV 2021*, 2021.
  <https://doi.org/10.1109/ICCV48922.2021.00963>
  - GMA. Evaluated in the AI-stitching study. Code and weights WTFPL.
- Zhaoyang Huang, Xiaoyu Shi, Chao Zhang et al. **FlowFormer: A Transformer
  Architecture for Optical Flow.** *ECCV 2022*, 2022.
  <https://doi.org/10.1007/978-3-031-19790-1_40>
  - Evaluated in the AI-stitching study. Code Apache-2.0.
- Xiaoyu Shi, Zhaoyang Huang, Dasong Li et al. **FlowFormer++: Masked Cost
  Volume Autoencoding for Pretraining Optical Flow Estimation.** *CVPR 2023*,
  2023. <https://doi.org/10.1109/CVPR52729.2023.00160>
  - Evaluated in the AI-stitching study. The README states Apache; the
    repository has no licence file.
- Haofei Xu, Jing Zhang, Jianfei Cai et al. **GMFlow: Learning Optical Flow
  via Global Matching.** *CVPR 2022*, 2022.
  <https://doi.org/10.1109/CVPR52688.2022.00795>
- Haofei Xu, Jing Zhang, Jianfei Cai et al. **Unifying Flow, Stereo and Depth
  Estimation.** *IEEE Transactions on Pattern Analysis and Machine
  Intelligence* 45(11), 2023. <https://doi.org/10.1109/TPAMI.2023.3298645>
  - Reference (both): the architecture family of DJI's learned flow network,
    as described in `NEURAL_STITCHING.md` section 2. UniMatch is also
    evaluated in the AI-stitching study (code MIT).
- Matteo Poggi, Fabio Tosi. **FlowSeek: Optical Flow Made Easier with Depth
  Foundation Models and Motion Bases.** *ICCV 2025*, 2025.
  <https://doi.org/10.1109/ICCV51701.2025.00537>
  - Evaluated in the AI-stitching study. Code Apache-2.0; its T and S
    variants embed Depth Anything V2 Small (Apache-2.0), its M and L variants
    Depth Anything V2 Base (CC BY-NC 4.0).
- David J. Heeger, Allan D. Jepson. **Subspace methods for recovering rigid
  motion I: Algorithm and implementation.** *International Journal of
  Computer Vision* 7(2), 1992. <https://doi.org/10.1007/BF00128130>
  - Reference: the source of the rigid-motion flow bases FlowSeek builds on
    (FlowSeek's reference [21]).
- Yihan Wang, Jia Deng. **WAFT: Warping-Alone Field Transforms for Optical
  Flow.** *ICLR 2026*, 2026. <https://openreview.net/forum?id=HTqGE0KcuF>
  - Evaluated: surveyed (`NEURAL_STITCHING.md` section 4) and measured in the
    AI-stitching study. Code BSD-3-Clause.
- Henrique Morimitsu, Xiaobin Zhu, Roberto M. Cesar et al. **DPFlow: Adaptive
  Optical Flow Estimation with a Dual-Pyramid Framework.** *CVPR 2025*, 2025.
  <https://doi.org/10.1109/CVPR52734.2025.01659>
  - Evaluated: surveyed and measured. Code Apache-2.0; weights research-only.
- Zhiyong Zhang, Aniket Gupta, Huaizu Jiang, Hanumant Singh. **NeuFlow v2:
  Push High-Efficiency Optical Flow To the Limit.** *IROS 2025*, 2025.
  <https://doi.org/10.1109/IROS60139.2025.11247683>
  - Evaluated: surveyed (the one candidate `NEURAL_STITCHING.md` section 4
    suggests for a quick A/B) and measured. Code and weights Apache-2.0.
- Vladislav Bargatin, Egor Chistov, Alexander Yakovenko, Dmitriy Vatolin.
  **MEMFOF: High-Resolution Training for Memory-Efficient Multi-Frame Optical
  Flow Estimation.** *ICCV 2025*, 2025.
  <https://doi.org/10.1109/ICCV51701.2025.00767>
  - Evaluated: surveyed. Code and weights BSD-3-Clause.
- Yuchen Zhang, Nikhil Keetha, Chenwei Lyu et al. **UFM: A Simple Path
  towards Unified Dense Correspondence with Flow.** *NeurIPS 2025*, 2025.
  <https://openreview.net/forum?id=Sv1pbyc8ZT>
  - Evaluated: surveyed. Code BSD-3-Clause; weights non-commercial.
- Hao Shi, Yifan Zhou, Kailun Yang et al. **PanoFlow: Learning 360° Optical
  Flow for Surrounding Temporal Understanding.** *IEEE Transactions on
  Intelligent Transportation Systems* 24(5), 2023.
  <https://doi.org/10.1109/TITS.2023.3241212>
  - Evaluated: surveyed (`NEURAL_STITCHING.md` section 4). Code MIT.
- Henrique Morimitsu. **PyTorch Lightning Optical Flow (ptlflow).** Software,
  2021. <https://github.com/hmorimitsu/ptlflow>
  - Reference: the runtime benchmark quoted in `NEURAL_STITCHING.md`
    section 4, and the loader the AI-stitching study uses. Code Apache-2.0;
    most of its converted checkpoints state no weights licence, which is one
    reason they are research-only here.

## 6. Monocular depth

- Lihe Yang, Bingyi Kang, Zilong Huang et al. **Depth Anything V2.** *NeurIPS
  2024*, 2024. <https://arxiv.org/abs/2406.09414>
  - Evaluated: a sky mask from far depth (`NEURAL_STITCHING.md` sections 3
    and 6), and depth-aware stitching in the AI-stitching study. Small:
    Apache-2.0; Base, Large and Giant: CC BY-NC 4.0.
- Ruicheng Wang, Sicheng Xu, Yue Dong et al. **MoGe-2: Accurate Monocular
  Geometry with Metric Scale and Sharp Details.** *NeurIPS 2025*, 2025.
  <https://arxiv.org/abs/2507.02546>
  - Evaluated in the AI-stitching study (metric depth). Code and ViT-S
    weights MIT.
- Aleksei Bochkovskii, Amaël Delaunoy, Hugo Germain et al. **Depth Pro: Sharp
  Monocular Metric Depth in Less Than a Second.** *ICLR 2025*, 2025.
  <https://openreview.net/forum?id=aueXfY0Clv>
  - Downloaded for the AI-stitching study. Apple sample-code licence on
    GitHub; the Hugging Face weights are research-only.

## 7. Harmonisation and segmentation

All evaluated in the photometry survey (`NEURAL_STITCHING.md` section 3);
none is used by OpenOSV.

- Wenyan Cong, Jianfu Zhang, Li Niu et al. **DoveNet: Deep Image
  Harmonization via Domain Verification.** *CVPR 2020*, 2020.
  <https://doi.org/10.1109/CVPR42600.2020.00842> (Also introduces the
  iHarmony4 dataset, whose terms are unclear.)
- Wenyan Cong, Li Niu, Jianfu Zhang et al. **BargainNet: Background-Guided
  Domain Translation for Image Harmonization.** *IEEE ICME 2021*, 2021.
  <https://doi.org/10.1109/ICME51207.2021.9428394>
- Zonghui Guo, Haiyong Zheng, Yufeng Jiang et al. **Intrinsic Image
  Harmonization.** *CVPR 2021*, 2021.
  <https://doi.org/10.1109/CVPR46437.2021.01610>
- Ben Xue, Shenghui Ran, Quan Chen et al. **DCCF: Deep Comprehensible Color
  Filter Learning Framework for High-Resolution Image Harmonization.** *ECCV
  2022*, 2022. <https://doi.org/10.1007/978-3-031-20071-7_18>
- Julian Jorge Andrade Guerreiro, Mitsuru Nakazawa, Björn Stenger. **PCT-Net:
  Full Resolution Image Harmonization Using Pixel-Wise Color
  Transformations.** *CVPR 2023*, 2023.
  <https://doi.org/10.1109/CVPR52729.2023.00573>
- Jianqi Chen, Yilan Zhang, Zhengxia Zou et al. **Dense Pixel-to-Pixel
  Harmonization via Continuous Image Representation.** *IEEE Transactions on
  Circuits and Systems for Video Technology* 34(5), 2024.
  <https://doi.org/10.1109/TCSVT.2023.3324591> (Code repository
  "INR-Harmonization".)
- Zhanghan Ke, Chunyi Sun, Lei Zhu et al. **Harmonizer: Learning to Perform
  White-Box Image and Video Harmonization.** *ECCV 2022*, 2022.
  <https://doi.org/10.1007/978-3-031-19784-0_40> (CC BY-NC-SA 4.0.)
- Wenyan Cong, Xinhao Tao, Li Niu et al. **High-Resolution Image
  Harmonization via Collaborative Dual Transformations.** *CVPR 2022*, 2022.
  <https://doi.org/10.1109/CVPR52688.2022.01792> (CDTNet.)
- Quanling Meng, Qinglin Liu, Zonglin Li et al. **High-Resolution Image
  Harmonization with Adaptive-Interval Color Transformation.** *NeurIPS
  2024*, 2024.
  <https://proceedings.neurips.cc/paper_files/paper/2024/hash/192956d4857000578f626c5193b34419-Abstract-Conference.html>
  (AICT.)
- Pengfei Zhou, Fangxiang Feng, Xiaojie Wang. **DiffHarmony: Latent
  Diffusion Model Meets Image Harmonization.** *ACM ICMR 2024*, 2024.
  <https://doi.org/10.1145/3652583.3657616>
- Zhengxia Zou, Rui Zhao, Tianyang Shi et al. **Castle in the Sky: Dynamic
  Sky Replacement and Harmonization in Videos.** *IEEE Transactions on Image
  Processing* 31, 2022. <https://doi.org/10.1109/TIP.2022.3192717> (SkyAR;
  CC BY-NC-SA 4.0.)
- Enze Xie, Wenhai Wang, Zhiding Yu et al. **SegFormer: Simple and Efficient
  Design for Semantic Segmentation with Transformers.** *NeurIPS 2021*, 2021.
  <https://arxiv.org/abs/2105.15203> (NVIDIA source licence, non-commercial.)
- Juncai Peng, Yi Liu, Shiyu Tang et al. **PP-LiteSeg: A Superior Real-Time
  Semantic Segmentation Model.** arXiv preprint, 2022.
  <https://arxiv.org/abs/2204.02681> (Code Apache-2.0; Cityscapes weights
  non-commercial.)
- Nikhila Ravi, Valentin Gabeur, Yuan-Ting Hu et al. **SAM 2: Segment
  Anything in Images and Videos.** *ICLR 2025*, 2025.
  <https://arxiv.org/abs/2408.00714> (Apache-2.0.)
- Bolei Zhou, Hang Zhao, Xavier Puig et al. **Scene Parsing through ADE20K
  Dataset.** *CVPR 2017*, 2017. <https://doi.org/10.1109/CVPR.2017.544>
- Marius Cordts, Mohamed Omran, Sebastian Ramos et al. **The Cityscapes
  Dataset for Semantic Urban Scene Understanding.** *CVPR 2016*, 2016.
  <https://doi.org/10.1109/CVPR.2016.350>
  - Reference (both datasets): their terms decide whether the segmentation
    weights trained on them could ship; both are non-commercial as recorded
    in the survey.

## 8. Inpainting and generative models

Evaluated for generative seam repair (`NEURAL_STITCHING.md` section 5); none
is used by OpenOSV.

- Andranik Sargsyan, Shant Navasardyan, Xingqian Xu, Humphrey Shi. **MI-GAN:
  A Simple Baseline for Image Inpainting on Mobile Devices.** *ICCV 2023*,
  2023. <https://doi.org/10.1109/ICCV51070.2023.00674> (MIT.)
- Roman Suvorov, Elizaveta Logacheva, Anton Mashikhin et al.
  **Resolution-robust Large Mask Inpainting with Fourier Convolutions.**
  *WACV 2022*, 2022. <https://doi.org/10.1109/WACV51458.2022.00323> (LaMa;
  the LaMa-ONNX export surveyed is Apache-2.0.)
- Robin Rombach, Andreas Blattmann, Dominik Lorenz et al. **High-Resolution
  Image Synthesis with Latent Diffusion Models.** *CVPR 2022*, 2022.
  <https://doi.org/10.1109/CVPR52688.2022.01042> (Basis of Stability AI's
  Stable Diffusion 2 inpainting model, which both diffusion stitchers above
  depend on; its official repository is no longer public.)
- Black Forest Labs. **FLUX.1 [schnell].** Model card, 2024.
  <https://huggingface.co/black-forest-labs/FLUX.1-schnell> (Apache-2.0.)
- Black Forest Labs. **FLUX.1 Fill [dev].** Model card, 2024.
  <https://huggingface.co/black-forest-labs/FLUX.1-Fill-dev> (FLUX.1 [dev]
  Non-Commercial License.)
- Junsong Chen, Shuchen Xue, Yuyang Zhao et al. **SANA-Sprint: One-Step
  Diffusion with Continuous-Time Consistency Distillation.** *ICCV 2025*,
  2025. <https://doi.org/10.1109/ICCV51701.2025.01502>
- Axel Sauer, Dominik Lorenz, Andreas Blattmann, Robin Rombach. **Adversarial
  Diffusion Distillation.** *ECCV 2024*, 2024.
  <https://doi.org/10.1007/978-3-031-73016-0_6> (SD-Turbo and SDXL-Turbo;
  Stability AI Community License.)
- Stability AI. **Stable Diffusion 3.5 Large Turbo.** Model card, 2024.
  <https://huggingface.co/stabilityai/stable-diffusion-3.5-large-turbo>
  (Stability AI Community License: commercial use above US$1M annual
  revenue needs an enterprise licence.)
- Yuxi Ren, Xin Xia, Yanzuo Lu et al. **Hyper-SD: Trajectory Segmented
  Consistency Model for Efficient Image Synthesis.** *NeurIPS 2024*, 2024.
  <https://arxiv.org/abs/2404.13686>
- Yufei Wang, Wenhan Yang, Xinyuan Chen et al. **SinSR: Diffusion-Based Image
  Super-Resolution in a Single Step.** *CVPR 2024*, 2024.
  <https://doi.org/10.1109/CVPR52733.2024.02437>
- Zongsheng Yue, Kang Liao, Chen Change Loy. **Arbitrary-steps Image
  Super-resolution via Diffusion Inversion.** *CVPR 2025*, 2025.
  <https://doi.org/10.1109/CVPR52734.2025.02156> (InvSR.)
- Shangchen Zhou, Chongyi Li, Kelvin C.K. Chan, Chen Change Loy.
  **ProPainter: Improving Propagation and Transformer for Video Inpainting.**
  *ICCV 2023*, 2023. <https://doi.org/10.1109/ICCV51070.2023.00961>
- Black Forest Labs. **FLUX.2 [klein] 4B.** Model release, 2026.
  <https://huggingface.co/black-forest-labs/FLUX.2-klein-4B>
  - Surveyed (`NEURAL_STITCHING.md` section 5) and downloaded for the
    AI-stitching study's generative experiments. Apache-2.0 (the 9B
    variants are non-commercial).
- Team Wan, Ang Wang, Baole Ai et al. **Wan: Open and Advanced Large-Scale
  Video Generative Models.** arXiv preprint, 2025.
  <https://arxiv.org/abs/2503.20314>
- Zeyinzi Jiang, Zhen Han, Chaojie Mao et al. **VACE: All-in-One Video
  Creation and Editing.** *ICCV 2025*, 2025.
  <https://doi.org/10.1109/ICCV51701.2025.01597>
  - Downloaded (both, as Wan2.1-VACE 1.3B) for the AI-stitching study's
    generative experiments. Apache-2.0.

## 9. Lens flare and veiling glare

From the flare research (`docs/research/FLARE.md` section 3). OpenOSV's ghost
removal is its own single-lens detector and parametric fit
(`include/osv/render/Flare.h`); none of the models below is used.

- Matthias B. Hullin, Elmar Eisemann, Hans-Peter Seidel, Sungkil Lee.
  **Physically-Based Real-Time Lens Flare Rendering.** *ACM Transactions on
  Graphics* 30(4) (SIGGRAPH 2011), 2011. <https://doi.org/10.1145/2010324.1965003>
- Sungkil Lee, Elmar Eisemann. **Practical Real-Time Lens-Flare Rendering.**
  *Computer Graphics Forum* 32(4), 2013. <https://doi.org/10.1111/cgf.12145>
  - Reference (both): ghost prediction from a lens prescription, and the
    ghost-line symmetry; not usable without DJI's lens design.
- Yuekun Dai, Yihang Luo, Shangchen Zhou et al. **Nighttime Smartphone
  Reflective Flare Removal Using Optical Center Symmetry Prior.** *CVPR
  2023*, 2023. <https://arxiv.org/abs/2303.15046>
  - Reference: the optical-centre symmetry prior, checked against the
    sample clip (it does not hold there). Evaluated: BracketFlare code and
    weights, S-Lab License 1.0, non-commercial.
- Zheyan Jin, Shiqi Chen, Huajun Feng et al. **Toward Real Flare Removal: A
  Comprehensive Pipeline and A New Benchmark.** arXiv preprint, 2023.
  <https://arxiv.org/abs/2306.15884>
  - Reference: protection-glass ghosts.
- Patricia Vitoria, Coloma Ballester. **Automatic Flare Spot Artifact
  Detection and Removal in Photographs.** *Journal of Mathematical Imaging
  and Vision* 61(4), 2019. <https://doi.org/10.1007/s10851-018-0859-0>
- Floris Chabert. **Automated Lens Flare Removal.** Stanford EE368 project
  report, 2015.
  <https://web.stanford.edu/class/ee368/Project_Autumn_1516/Reports/Chabert.pdf>
  - Reference (both): spot detection plus inpainting, the approach set aside
    for faint ghosts.
- Eino-Ville Talvala, Andrew Adams, Mark Horowitz, Marc Levoy. **Veiling
  Glare in High Dynamic Range Imaging.** *ACM Transactions on Graphics* 26(3)
  (SIGGRAPH 2007), 2007. <https://doi.org/10.1145/1276377.1276424>
  - Reference: the additive veiling-glare model behind the veil term
    (`FLARE.md`, `NEURAL_STITCHING.md` section 3).
- John J. McCann, Alessandro Rizzi. **Veiling glare: the dynamic range limit
  of HDR images.** *Proc. SPIE* 6492, Human Vision and Electronic Imaging
  XII, 2007. <https://doi.org/10.1117/12.703042>
- J. A. Seibert, O. Nalcioglu, W. Roeck. **Removal of image intensifier
  veiling glare by mathematical deconvolution techniques.** *Medical
  Physics* 12(3), 1985. <https://doi.org/10.1118/1.595720>
- Ramesh Raskar, Amit Agrawal, Cyrus A. Wilson, Ashok Veeraraghavan. **Glare
  Aware Photography: 4D Ray Sampling for Reducing Glare Effects of Camera
  Lenses.** *ACM Transactions on Graphics* 27(3) (SIGGRAPH 2008), 2008.
  <https://doi.org/10.1145/1360612.1360655>
  - Reference (these three): why veiling glare cannot simply be deconvolved.
- ISO. **ISO 9358:1994, Optics and optical instruments — Veiling glare of
  image forming systems — Definitions and methods of measurement.** 1994.
  <https://www.iso.org/standard/17042.html>
- ISO. **ISO 18844:2017, Photography — Digital cameras — Image flare
  measurement.** 2017. <https://www.iso.org/standard/63552.html>
  - Reference (both): the uniform veil (veiling glare index) model that the
    kernel's veil term follows.
- Yicheng Wu, Qiurui He, Tianfan Xue et al. **How to Train Neural Networks
  for Flare Removal.** *ICCV 2021*, 2021. <https://arxiv.org/abs/2011.12485>
  - Reference: masking the saturated source and adding it back, the
    principle OpenOSV's ghost removal follows around the sun. Evaluated:
    code Apache-2.0, flare images CC BY 4.0, no pretrained model released.
    The training method is patented (section 14).
- Yuekun Dai, Chongyi Li, Shangchen Zhou et al. **Flare7K: A
  Phenomenological Nighttime Flare Removal Dataset.** *NeurIPS 2022 Datasets
  and Benchmarks*, 2022. <https://arxiv.org/abs/2210.06570>
- Yuekun Dai, Chongyi Li, Shangchen Zhou et al. **Flare7K++: Mixing Synthetic
  and Real Datasets for Nighttime Flare Removal and Beyond.** *IEEE
  Transactions on Pattern Analysis and Machine Intelligence* 46(11), 2024.
  <https://doi.org/10.1109/TPAMI.2024.3406821>
  - Evaluated (both): S-Lab License 1.0, non-commercial; not shippable.
- Yuyan Zhou, Dong Liang, Songcan Chen et al. **Improving Lens Flare Removal
  with General-Purpose Pipeline and Multiple Light Sources Recovery.** *ICCV
  2023*, 2023. <https://arxiv.org/abs/2308.16460>
  - Evaluated: no licence on code or weights; not shippable. One co-author is
    from DJI's imaging group.
- Yuekun Dai, Dafeng Zhang, Xiaoming Li et al. **MIPI 2024 Challenge on
  Nighttime Flare Removal: Methods and Results.** *CVPR Workshops 2024*,
  2024. <https://arxiv.org/abs/2404.19534>
  - Evaluated: the FlareReal600 dataset it introduces, CC BY-NC-SA 4.0.
- Yousef Kotp, Marwan Torki. **Flare-Free Vision: Empowering Uformer with
  Depth Insights.** *IEEE ICASSP 2024*, 2024.
  <https://doi.org/10.1109/ICASSP48485.2024.10446006>
  - Evaluated: S-Lab License 1.0, non-commercial.
- Lishen Qu, Zhihao Liu, Jinshan Pan et al. **FlareX: A Physics-Informed
  Dataset for Lens Flare Removal via 2D Synthesis and 3D Rendering.**
  *NeurIPS 2025 Datasets and Benchmarks*, 2025.
  <https://arxiv.org/abs/2510.09995>
  - Evaluated: code GPL-3.0; dataset licence not stated.
- Jie Zhu, Sungkil Lee. **PBFG: A New Physically-Based Dataset and Removal of
  Lens Flares and Glares.** *ICCV 2025*, 2025.
  <https://openaccess.thecvf.com/content/ICCV2025/html/Zhu_PBFG_A_New_Physically-Based_Dataset_and_Removal_of_Lens_Flares_ICCV_2025_paper.html>
  - Evaluated: CC BY-NC-SA 4.0 (stated in the README).
- Xiaolong Qian, Qi Jiang, Lei Sun et al. **Learning Latent Transmission and
  Glare Maps for Lens Veiling Glare Removal.** *CVPR 2026*, 2026.
  <https://arxiv.org/abs/2511.17353>
  - Evaluated: DeVeiler; weights not released.
- Gopi Raju Matta, Rahul Siddartha, Rongali Simhachala Venkata Girish et al.
  **GN-FR: Generalizable Neural Radiance Fields for Flare Removal.** *BMVC
  2024*, 2024. <https://arxiv.org/abs/2412.08200>
  - Evaluated: multi-view flare removal; offline, no code found.

## 10. Lens model, geometry and numerical methods

- Juho Kannala, Sami S. Brandt. **A generic camera model and calibration
  method for conventional, wide-angle, and fish-eye lenses.** *IEEE
  Transactions on Pattern Analysis and Machine Intelligence* 28(8), 2006.
  <https://doi.org/10.1109/TPAMI.2006.153>
  - Implemented: the 5-term fisheye lens model the camera's calibration uses
    (`include/osv/geom/KannalaBrandt5.h`, `docs/GEOMETRY.md`).
- Ken Shoemake. **Animating rotation with quaternion curves.** *ACM SIGGRAPH
  Computer Graphics* 19(3) (SIGGRAPH '85), 1985.
  <https://doi.org/10.1145/325165.325242>
  - Implemented: quaternion slerp for IMU attitude interpolation
    (`include/osv/core/Math.h`, `src/osv/geom/AttitudeTrack.cpp`).
- F. N. Fritsch, R. E. Carlson. **Monotone Piecewise Cubic Interpolation.**
  *SIAM Journal on Numerical Analysis* 17(2), 1980.
  <https://doi.org/10.1137/0717021>
  - Implemented: monotone tangents of the look's tone curve
    (`include/osv/color/Look.h`, `src/osv/color/Look.cpp`,
    `include/osv/color/ColorMath.h`).
- Kenneth Levenberg. **A method for the solution of certain non-linear
  problems in least squares.** *Quarterly of Applied Mathematics* 2(2), 1944.
  <https://doi.org/10.1090/qam/10666>
- Donald W. Marquardt. **An Algorithm for Least-Squares Estimation of
  Nonlinear Parameters.** *Journal of the Society for Industrial and Applied
  Mathematics* 11(2), 1963. <https://doi.org/10.1137/0111030>
  - Implemented (both): the ghost fit in the flare analysis
    (`include/osv/render/Flare.h`, `src/osv/render/Flare.cpp`) and the
    D-Log M curve fit (`scripts/fit_dlogm.py`).
- G. H. Golub, V. Pereyra. **The Differentiation of Pseudo-Inverses and
  Nonlinear Least Squares Problems Whose Variables Separate.** *SIAM Journal
  on Numerical Analysis* 10(2), 1973. <https://doi.org/10.1137/0710036>
  - Implemented: variable projection in the flare ghost fit (linear terms
    solved inside each nonlinear step, `Flare.h`).
- B. P. Welford. **Note on a Method for Calculating Corrected Sums of Squares
  and Products.** *Technometrics* 4(3), 1962.
  <https://doi.org/10.1080/00401706.1962.10490022>
  - Implemented: running statistics in the attitude convention probe
    (`src/osv/geom/ConventionProbe.cpp`).
- Bruce D. Lucas, Takeo Kanade. **An Iterative Image Registration Technique
  with an Application to Stereo Vision.** *IJCAI '81*, 1981.
  <https://www.ijcai.org/Proceedings/81-2/Papers/017.pdf>
  - Implemented in tests only: sub-pixel tile alignment that checks
    rendered framing (`tests/premiere/reframe/GpuTestSupport.cpp`).

## 11. Colour standards and colour science

For the ITU-R documents, "used" is the revision the code was written from
(the one `NOTICE` names); "current" is the revision in force on 2026-09-23.

- ITU-R. **Recommendation BT.2100: Image parameter values for high dynamic
  range television for use in production and international programme
  exchange.** Used BT.2100-2 (07/2018); current BT.2100-3 (02/2025).
  <https://www.itu.int/rec/R-REC-BT.2100-3-202502-I/en>
  - Implemented: the HLG OETF and OOTF and the PQ EOTF and its inverse
    (`include/osv/color/ColorMath.h`).
- ITU-R. **Recommendation BT.2020: Parameter values for ultra-high definition
  television systems for production and international programme exchange.**
  BT.2020-2 (10/2015), current. <https://www.itu.int/rec/R-REC-BT.2020-2-201510-I/en>
  - Implemented: the Rec.2020 working primaries.
- ITU-R. **Recommendation BT.709: Parameter values for the HDTV standards for
  production and international programme exchange.** BT.709-6 (06/2015),
  current. <https://www.itu.int/rec/R-REC-BT.709-6-201506-I/en>
  - Implemented: Rec.709 primaries and OETF, and the YCbCr matrix the camera's
    streams are decoded with.
- ITU-R. **Recommendation BT.2087: Colour conversion from Recommendation ITU-R
  BT.709 to Recommendation ITU-R BT.2020.** BT.2087-0 (10/2015), current.
  <https://www.itu.int/rec/R-REC-BT.2087-0-201510-I/en>
  - Implemented: `kRec2020ToRec709` (`include/osv/color/Matrices.h`).
- ITU-R. **Recommendation BT.1886: Reference electro-optical transfer
  function for flat panel displays used in HDTV studio production.**
  BT.1886-0 (03/2011), current. <https://www.itu.int/rec/R-REC-BT.1886-0-201103-I/en>
  - Implemented: the display model under the look's perceptual fit and tests
    (`scripts/fit_look.py`, `tests/unit/test_look.cpp`).
- ITU-R. **Recommendation BT.601: Studio encoding parameters of digital
  television for standard 4:3 and wide screen 16:9 aspect ratios.**
  BT.601-7 (03/2011), current. <https://www.itu.int/rec/R-REC-BT.601-7-201103-I/en>
  - Reference: the YCbCr encoding DJI's seam compensation works in, as
    described in `NEURAL_STITCHING.md` section 2.
- ITU-R. **Report BT.2408: Guidance for operational practices in HDR
  television production** (retitled "Guidelines for operational practices in
  high dynamic range television production" in -9). Used BT.2408-5
  (03/2022); current BT.2408-9 (03/2026). <https://www.itu.int/pub/R-REP-BT.2408>
  - Implemented: the signal-level anchors, 18 % grey at 38 % HLG and diffuse
    white at 75 % HLG, which set the scene scale 0.2674
    (`include/osv/color/ColorMath.h`, `docs/COLOR.md`).
  - Implemented: the Annex 5 reference EETF ("Displaying PQ - calculating the
    EETF", read in BT.2408-7, 2023), applied per R'G'B' component, behind the
    PQ output's HDR Peak Brightness setting (`osvHdrPeakRolloff`,
    `docs/COLOR.md` "HDR peak brightness").
  - Implemented: the display-light anchors (18 % grey at 26 nits, HDR
    reference white at 203 nits) that the Transfer Function (HDR) styles
    are pinned to: grey for ACES 2 Bright and the two Deep Blacks styles,
    grey and diffuse white for the Deep Blacks styles, and the BT.2408
    Neutral style is the scene-referred rendering above (`setHdrTone`,
    `docs/COLOR.md` "Transfer Function (HDR)").
- ITU-R. **Report BT.2390: High dynamic range television for production and
  international programme exchange.** Used BT.2390-8 (02/2020); current
  BT.2390-12 (03/2025). <https://www.itu.int/pub/R-REP-BT.2390>
  - Implemented: the EETF for PQ tone mapping (BT.2390-8 section 5.4.1; from
    2021 that material lives in BT.2408 Annex 5), and the "HLG on an SDR
    display" rendering behind the standard Rec.709 output
    (`ColorMath.h`, `docs/COLOR.md`).
  - Reference: the HLG system gamma for a display of peak Lw,
    1.2 + 0.42 log10(Lw / 1000), and the 400-2000 nit range it is validated
    over (section 6.2, read in BT.2390-11) - the reason the HDR peak setting
    leaves the display-relative HLG output alone (`docs/COLOR.md`).
- SMPTE. **ST 2084:2014, High Dynamic Range Electro-Optical Transfer Function
  of Mastering Reference Displays.** 2014.
  <https://doi.org/10.5594/SMPTE.ST2084.2014>
  - Implemented: the PQ constants and curve (`ColorMath.h`).
- ARIB. **STD-B67, Parameter Values for the Hybrid Log-Gamma (HLG) High
  Dynamic Range Television (HDR-TV) System for Programme Production.**
  Version 2.0, 2018 (Version 1.0, 2015, was titled "Essential Parameter
  Values for the Extended Image Dynamic Range Television (EIDRTV) System for
  Programme Production").
  <https://www.arib.or.jp/english/std_tr/broadcasting/std-b67.html>
  - Reference: the HLG transfer identifier OpenOSV writes when it tags its
    output (`src/osv/io/FfmpegPipe.cpp`); the curve is implemented from
    BT.2100.
- Academy of Motion Picture Arts and Sciences. **Reference Gamut Compression
  (RGC) Specification.** ACES 1.3, 2021.
  <https://docs.acescentral.com/rgc/specification/>
  - Implemented: the compression curve in the look's soft gamut compression
    (`osvLookApply` in `ColorMath.h`), from its mathematical description; no
    ACES code is included.
- Contributors to the ACES Project (Academy Software Foundation).
  **aces-core: `lib/Lib.Academy.Tonescale.ctl`**, the ACES 2.0 tonescale
  (`tonescale_fwd`, `init_TSParams`). ACES 2.0, 2024-2025. Apache-2.0.
  <https://github.com/aces-aswf/aces-core/blob/main/lib/Lib.Academy.Tonescale.ctl>
  - Implemented: the tonescale's Michaelis-Menten form with the flare term,
    f = m_2 (x / (x + s_2))^g, h = max(0, f^2 / (f + t_1)), display light
    100 h nits, behind the Transfer Function (HDR) styles (`osvHdrToneCurve`
    and `osvHdrToneApply` in `ColorMath.h`, constants in
    `src/osv/color/ColorParams.cpp`; attribution in `NOTICE`). The ACES 2
    styles' g, c_d, t_1 and r_hit were least-squares fitted to the neutral
    axis of DJI's published D-Log M to Rec.709 LUT through `init_TSParams`
    and its peak rule r_hit(n) = r_hit(100) (1 + 3 log10(n / 100)); the
    BT.2408 styles keep that t_1. Only the tonescale is used: none of the
    ACES 2.0 Output Transform's other stages (the JMh chroma compression and
    gamut mapping) are implemented (`docs/COLOR.md` "Transfer Function
    (HDR)").
- Daniele Siragusano. **Output Transform Tone Scale** (proposal in the ACES
  Output Transforms virtual working group). ACESCentral community, 2021.
  <https://community.acescentral.com/t/output-transform-tone-scale/3498/14>
  - Reference: the Michaelis-Menten tone scale with a surround gamma from
    which the ACES 2.0 tonescale above was developed.
- CIE. **CIE 015:2018, Colorimetry, 4th Edition.** 2018.
  <https://doi.org/10.25039/TR.015.2018>
  - Implemented: CIE 1976 L\*a\*b\* in the look's fit and tests.
- CIE. **CIE 142:2001, Improvement to industrial colour-difference
  evaluation.** 2001.
  <https://cie.co.at/publications/improvement-industrial-colour-difference-evaluation>
- Gaurav Sharma, Wencheng Wu, Edul N. Dalal. **The CIEDE2000 color-difference
  formula: Implementation notes, supplementary test data, and mathematical
  observations.** *Color Research & Application* 30(1), 2005.
  <https://doi.org/10.1002/col.20070>
  - Implemented (both): the CIEDE2000 colour difference that scores the look
    (`scripts/fit_look.py`, `tests/unit/test_look.cpp`).
- Thatcher Freeman. **DJI Pocket 3 D-Log M to DWG.dctl**, in *dwg-transforms*
  (GitHub repository).
  <https://github.com/thatcherfreeman/dwg-transforms/blob/main/RCM%20IDTs/DJI%20Pocket%203%20D-Log%20M%20to%20DWG.dctl>
  - Implemented: its published D-Log M curve constants (`kDlogMPocket3` in
    `include/osv/color/DlogM.h`) and its colour-chart matrix, converted to
    Rec.2020 (`kNativeToRec2020_Pocket3` in `include/osv/color/Matrices.h`).
    The repository has no licence file, so only the numeric constants are
    reproduced; no code from it is included.
- Blackmagic Design. **DaVinci Resolve 17 Wide Gamut Intermediate.**
  Information note, v1.1, 2021.
  <https://documents.blackmagicdesign.com/InformationNotes/DaVinci_Resolve_17_Wide_Gamut_Intermediate.pdf>
  - Reference: the DaVinci Wide Gamut primaries through which Freeman's
    matrix was converted to Rec.2020.

## 12. File formats and codecs

- ISO/IEC. **14496-12, Information technology — Coding of audio-visual
  objects — Part 12: ISO base media file format.** Current edition 2026.
  <https://www.iso.org/standard/85596.html>
  - Implemented: the box walker and sample tables (`include/osv/container/*`).
- ISO/IEC. **14496-15, Part 15: Carriage of network abstraction layer (NAL)
  unit structured video in the ISO base media file format.** Current
  edition 2024. <https://www.iso.org/standard/89118.html>
  - Implemented: `hvcC` and `avcC` parsing (`include/osv/container/HevcConfig.h`).
- ISO/IEC. **14496-3, Part 3: Audio.** Current edition 2019.
  <https://www.iso.org/standard/76383.html>
  - Implemented: the AAC sampling-frequency index table
    (`src/osv/video/StreamExtract.cpp`).
- ITU-T. **Recommendation H.265, High efficiency video coding.** Current
  edition 01/2026. <https://www.itu.int/rec/T-REC-H.265-202601-I/en>
- ITU-T. **Recommendation H.264, Advanced video coding for generic
  audiovisual services.** Current edition 06/2026.
  <https://www.itu.int/rec/T-REC-H.264-202606-I/en>
  - Reference (both): the camera's HEVC streams and the `.LRF` proxy's H.264
    stream. Decoding is done by FFmpeg (`NOTICE`).
- Google. **Protocol Buffers: Encoding.** Web documentation.
  <https://protobuf.dev/programming-guides/encoding/>
  - Implemented: the hand-written wire-format decoder for the `djmd` metadata
    (`include/osv/meta/ProtoWire.h`, `src/osv/meta/DjmdDecoder.cpp`).

## 13. Open-source projects used as references

No code from these projects is included in OpenOSV.

- Adrian Eddy et al. **telemetry-parser.** GitHub repository.
  <https://github.com/AdrianEddy/telemetry-parser>
  - Reference: its `src/dji/dvtm_oq101.proto` is the published schema the
    `djmd` metadata transcription was cross-checked against
    (`proto/dvtm_osmo360.proto`, `docs/LEGAL.md` section 5.5). MIT OR
    Apache-2.0.
- Facebook. **Surround 360.** GitHub repository (archived), 2016.
  <https://github.com/facebook/Surround360>
  - Reference: its novel-view warp and deghosting blend
    (`NovelViewUtil::combineNovelViews`; its two blend constants, 10 and 10,
    are the starting values of `DeghostParams` in
    `include/osv/render/FlowWarp.h`), and its separate horizontal and
    vertical flow regularisation (`include/osv/render/ParallaxWarp.h`).
    Render code BSD-3-Clause with an additional patent grant
    (`surround360_render/PATENTS.md`).
- OpenCV team. **OpenCV.** <https://opencv.org>
  - Reference: `cv::DISOpticalFlow`, which the DIS solver was checked against
    in spirit (`DisFlow.h`) and whose presets the AI-stitching study measures
    as baselines; the stitching module's `MultiBandBlender` and
    `GainCompensator`, in the survey (`NEURAL_STITCHING.md` section 3).
    Apache-2.0 from 4.5.0 (3-clause BSD before). OpenCV is not linked into
    OpenOSV; research scripts use it through Python.

## 14. Patents reviewed

Found during the flare research and recorded in `docs/research/FLARE.md`
section 3.4, which explains which code they affect. This list is not a
freedom-to-operate review (`docs/LEGAL.md` section 9). Status as shown on
Google Patents on 2026-09-23.

- GoPro, Inc. **US10630921B2, Image signal processing for reducing lens
  flare.** Active, anticipated expiry 2038.
  <https://patents.google.com/patent/US10630921B2/en>
- GoPro, Inc. **US11330208B2, Image signal processing for reducing lens
  flare** (a continuation of US10630921B2). Active, anticipated expiry 2038.
  <https://patents.google.com/patent/US11330208B2/en>
- GoPro, Inc. **US11503232B2, Image signal processing for reducing lens
  flare** (a separate family with the same title). Active, anticipated
  expiry 2040. <https://patents.google.com/patent/US11503232B2/en>
- GoPro, Inc. **US12439177B2, Flare compensation.** Active, anticipated
  expiry 2044. <https://patents.google.com/patent/US12439177B2/en>
  - (The four GoPro patents) Why OpenOSV's two-lens veil estimator is
    implemented but not switched on until cleared.
- Apple Inc. **US9860446B2, Flare detection and mitigation in panoramic
  images.** Expired for non-payment of fees, effective 2026-01-02.
  <https://patents.google.com/patent/US9860446B2/en>
  - Steering the seam away from flare (`flareCost()` in
    `include/osv/render/Flare.h`).
- Google LLC. **US12033309B2, Learning-based lens flare removal.** Active,
  anticipated expiry 2041. <https://patents.google.com/patent/US12033309B2/en>
  - Covers the synthetic-pair training method of Wu et al. (section 9);
    relevant only if OpenOSV trains its own flare model.
