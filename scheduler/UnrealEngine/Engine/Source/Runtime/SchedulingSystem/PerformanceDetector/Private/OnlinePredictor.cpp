#include "OnlinePredictor.h"
#include <algorithm>
#include <cmath>

FOnlinePredictor::FOnlinePredictor(int InHistoryLength, double InForgettingFactor)
    : ForgettingFactor(std::isfinite(InForgettingFactor) && InForgettingFactor > 0 && InForgettingFactor <= 1 ? InForgettingFactor : .98)
{
    FeatureSize = std::max(1, InHistoryLength) + 1;
    CurrentWeights.resize(FeatureSize, 0.0);

    // rural
    // std::vector<double> Weights = {1.469337, -0.28733, -0.22863, 8.787756}; // weights={w3,w2,w1,d} BlackMyth-200apm
    // std::vector<double> Weights = {1.404611, -0.20713, -0.24817, 6.368396}; // weights={w3,w2,w1,d} BlackMyth-300apm
    // std::vector<double> Weights = {1.108417, -0.11725, -0.0443, 8.51669};   // weights={w3,w2,w1,d} FFXVI-200apm
    // std::vector<double> Weights = {1.171115, -0.07491, -0.19473, 10.52871}; // weights={w3,w2,w1,d} FFXVI-300apm
    // std::vector<double> Weights = {1.451419, -0.24936, -0.25198, 8.69996};  // weights={w3,w2,w1,d} RDR2-200apm
    // std::vector<double> Weights = {1.416931, -0.22387, -0.25057, 6.681748}; // weights={w3,w2,w1,d} RDR2-300apm

    // watermills
    // std::vector<double> Weights = {0.928304, 0.00629, -0.08938, 18.14008};  // weights={w3,w2,w1,d} BlackMyth-200apm
    // std::vector<double> Weights = {0.873371, 0.119535, -0.05704, 11.27746}; // weights={w3,w2,w1,d} BlackMyth-300apm
    // std::vector<double> Weights = {0.910026, 0.073817, -0.03962, 8.971267}; // weights={w3,w2,w1,d} RDR2-200apm
    // std::vector<double> Weights = {0.946492, 0.002635, -0.07186, 13.15483}; // weights={w3,w2,w1,d} RDR2-300apm

    // Start untrained instead of importing another game's/GPU's coefficients.

    // Initialize P matrix
    P_Matrix.resize(FeatureSize, std::vector<double>(FeatureSize, 0.0));
    for (int i = 0; i < FeatureSize; i++)
    {
        P_Matrix[i][i] = 1.0;
    }
}

FOnlinePredictor::~FOnlinePredictor()
{
}

std::vector<double> FOnlinePredictor::MakeFeatures(const std::vector<double>& HistoryWindow)
{
    std::vector<double> Features;
    // Reverse extract: [t-1, t-2, t-3]
    for (int i = (int)HistoryWindow.size() - 1; i >= 0 && Features.size() < (size_t)(FeatureSize - 1); --i)
    {
        Features.push_back(HistoryWindow[i]);
    }

    while (Features.size() < (size_t)(FeatureSize - 1))
    {
        Features.push_back(0.0);
    }

    Features.push_back(1.0); // Bias term
    return Features;
}

double FOnlinePredictor::Predict(const std::vector<double>& HistoryWindow)
{
    std::vector<double> Features = MakeFeatures(HistoryWindow);
    double Result = 0.0;

    for (int i = 0; i < FeatureSize; i++)
    {
        Result += CurrentWeights[i] * Features[i];
    }

    return std::isfinite(Result) ? std::max(Result, 0.0) : 0.0;
}

void FOnlinePredictor::Update(double ActualValue, const std::vector<double>& HistoryWindow)
{
    if (!std::isfinite(ActualValue) || ActualValue < 0 ||
        std::any_of(HistoryWindow.begin(), HistoryWindow.end(), [](double Value) { return !std::isfinite(Value) || Value < 0; })) return;
    std::vector<double> Features = MakeFeatures(HistoryWindow);

    // 1. Prediction error
    double Prediction = 0.0;
    for (int i = 0; i < FeatureSize; i++)
    {
        Prediction += CurrentWeights[i] * Features[i];
    }
    double Error = ActualValue - Prediction;

    // 2. Gain Vector: K = (P * x) / (lambda + x^T * P * x)
    std::vector<double> Px(FeatureSize, 0.0);
    for (int i = 0; i < FeatureSize; i++)
    {
        for (int j = 0; j < FeatureSize; j++)
        {
            Px[i] += P_Matrix[i][j] * Features[j];
        }
    }

    double Denominator = ForgettingFactor;
    for (int i = 0; i < FeatureSize; i++)
    {
        Denominator += Features[i] * Px[i];
    }

    if (!std::isfinite(Denominator) || std::abs(Denominator) < 1e-12) return;

    std::vector<double> K(FeatureSize);
    for (int i = 0; i < FeatureSize; i++)
    {
        K[i] = Px[i] / Denominator;
    }

    // 3. Update W = W + K * Error
    for (int i = 0; i < FeatureSize; i++)
    {
        CurrentWeights[i] += K[i] * Error;
    }

    // 4. Update P = (P - K * x^T * P) / lambda
    std::vector<std::vector<double>> NewP = P_Matrix;
    for (int i = 0; i < FeatureSize; i++)
    {
        for (int j = 0; j < FeatureSize; j++)
        {
            double Term = K[i] * Px[j];
            NewP[i][j] = (P_Matrix[i][j] - Term) / ForgettingFactor;
        }
    }
    P_Matrix = NewP;
}
