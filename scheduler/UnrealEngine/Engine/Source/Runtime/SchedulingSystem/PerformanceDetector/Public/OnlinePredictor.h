#pragma once

#include <vector>

class FOnlinePredictor
{
public:
    FOnlinePredictor(int InHistoryLength, double InForgettingFactor = 0.98);
    ~FOnlinePredictor();

    double Predict(const std::vector<double>& HistoryWindow);

    void Update(double ActualValue, const std::vector<double>& HistoryWindow);

private:

    std::vector<double> MakeFeatures(const std::vector<double>& HistoryWindow);

    int FeatureSize;
    double ForgettingFactor;

    std::vector<double> CurrentWeights;        // Weight vector (W)
    std::vector<std::vector<double>> P_Matrix; // Covariance matrix (P)
};
