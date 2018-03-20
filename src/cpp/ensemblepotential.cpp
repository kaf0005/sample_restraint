//
// Created by Eric Irrgang on 2/26/18.
//

#include "ensemblepotential.h"

#include <vector>

namespace plugin
{

// Explicit instantiation.
template class ::plugin::Matrix<double>;

void EnsembleResourceHandle::reduce(const ::plugin::Matrix<double>& send, ::plugin::Matrix<double>* receive) const
{
    (*_reduce)(send, receive);
}

template<typename T_I, typename T_O>
void EnsembleResourceHandle::map_reduce(const T_I &iterable,
                                        T_O *output,
                                        void (*function)(double, const PairHist & input,
                                                 PairHist * output)
                                        )
{}

/*!
 * \brief Apply a Gaussian blur when building a density grid for a list of values.
 *
 * Normalize such that the area under each sample is 1.0/num_samples.
 */
class BlurToGrid
{
    public:
        BlurToGrid(double min_dist, double max_dist, double sigma) :
            _min_dist{min_dist},
            _max_dist{max_dist},
            _sigma{sigma}
        {
        };

        void operator() (const std::vector<double>& distances, std::vector<double>* grid)
        {
            const auto nbins = grid->size();
            const double dx{(_max_dist - _min_dist)/nbins};
            const auto num_samples = distances.size();

            const double denominator = 1.0/(2*_sigma*_sigma);
            const double normalization = 1.0/(num_samples*sqrt(2.0*M_PI*_sigma*_sigma));
            // We aren't doing any filtering of values too far away to contribute meaningfully, which
            // is admittedly wasteful for large sigma...
            for (size_t i = 0; i < nbins; ++i)
            {
                double bin_value{0};
                const double bin_x{i*dx};
                for(const auto distance : distances)
                {
                    const double relative_distance{bin_x - distance};
                    const auto numerator = -relative_distance*relative_distance;
                    bin_value += normalization*exp(numerator*denominator);
                }
                grid->at(i) = bin_value;
            }
        };

    private:
        /// Minimum value of bin zero
        const double _min_dist;
        /// Maximum value of bin
        const double _max_dist;
        const double _sigma;
};

EnsembleHarmonic::EnsembleHarmonic(size_t nbins,
                                   double min_dist,
                                   double max_dist,
                                   const PairHist &experimental,
                                   unsigned int nsamples,
                                   double sample_period,
                                   unsigned int nwindows,
                                   double window_update_period,
                                   double K,
                                   double sigma) :
    nBins_{nbins},
    minDist_{min_dist},
    maxDist_{max_dist},
    binWidth_{(maxDist_ - minDist_)/nBins_},
    histogram_(nBins_, 0),
    experimental_{experimental},
    nSamples_{nsamples},
    currentSample_{0},
    samplePeriod_{sample_period},
    nextSampleTime_{samplePeriod_},
    distanceSamples_(nSamples_),
    nWindows_{nwindows},
    currentWindow_{0},
    windowUpdatePeriod_{window_update_period},
    nextWindowUpdateTime_{windowUpdatePeriod_},
    windows_(),
    k_{K},
    sigma_{sigma}
{
    // We leave _histogram and _experimental unallocated until we have valid data to put in them, so that
    // (_histogram == nullptr) == invalid histogram.
}

EnsembleHarmonic::EnsembleHarmonic(const input_param_type &params) :
    EnsembleHarmonic(params.nbins,
                     params.min_dist,
                     params.max_dist,
                     params.experimental,
                     params.nsamples,
                     params.sample_period,
                     params.nwindows,
                     params.window_update_period,
                     params.K,
                     params.sigma)
{
}

void EnsembleHarmonic::callback(gmx::Vector v,
                                gmx::Vector v0,
                                double t,
                                const EnsembleResources &resources)
{
    auto rdiff = v - v0;
    const auto Rsquared = dot(rdiff,
                              rdiff);
    const auto R = sqrt(Rsquared);

    // Store historical data every sample_period steps
    {
        std::lock_guard<std::mutex> lock(samples_mutex_);
        if (t >= nextSampleTime_)
        {
            distanceSamples_[currentSample_++] = R;
            nextSampleTime_ += samplePeriod_;
        };
    }

    // Every nsteps:
    //   0. Drop oldest window
    //   1. Reduce historical data for this restraint in this simulation.
    //   2. Call out to the global reduction for this window.
    //   3. On update, checkpoint the historical data source.
    //   4. Update historic windows.
    //   5. Use handles retained from previous windows to reconstruct the smoothed working histogram
    {
        std::lock_guard<std::mutex> lock_windows(windows_mutex_);
        // Since we reset the samples state at the bottom, we should probably grab the mutex here for
        // better exception safety.
        std::lock_guard<std::mutex> lock_samples(samples_mutex_);
        if (t >= nextWindowUpdateTime_)
        {
            // Get next histogram array, recycling old one if available.
            std::unique_ptr<Matrix<double>> new_window = gmx::compat::make_unique<Matrix<double>>(1,
                                                                                                  nBins_);
            std::unique_ptr<Matrix<double>> temp_window;
            if (windows_.size() == nWindows_)
            {
                // Recycle the oldest window.
                // \todo wrap this in a helper class that manages a buffer we can shuffle through.
                windows_[0].swap(temp_window);
                windows_.erase(windows_.begin());
            }
            else
            {
                auto new_temp_window = gmx::compat::make_unique<Matrix<double>>(1,
                                                                                nBins_);
                temp_window.swap(new_temp_window);
            }

            // Reduce sampled data for this restraint in this simulation, applying a Gaussian blur to fill a grid.
            auto blur = BlurToGrid(minDist_,
                                   maxDist_,
                                   sigma_);
            assert(new_window != nullptr);
            blur(distanceSamples_,
                 new_window->vector());
            // We can just do the blur locally since there aren't many bins. Bundling these operations for
            // all restraints could give us a chance at some parallelism. We should at least use some
            // threading if we can.

            // We request a handle each time before using resources to make error handling easier if there is a failure in
            // one of the ensemble member processes and to give more freedom to how resources are managed from step to step.
            auto ensemble = resources.getHandle();
            // Get global reduction (sum) and checkpoint.
            assert(temp_window != nullptr);
            ensemble.reduce(*new_window,
                            temp_window.get());

            // Update window list with smoothed data.
            windows_.emplace_back(std::move(new_window));

            // Get new histogram difference. Subtract the experimental distribution to get the values to use in our potential.
            for (auto &bin : histogram_)
            {
                bin = 0;
            }
            for (const auto &window : windows_)
            {
                for (size_t i = 0; i < window->cols(); ++i)
                {
                    histogram_.at(i) += window->vector()->at(i) - experimental_.at(i);
                }
            }


            // Note we do not have the integer timestep available here. Therefore, we can't guarantee that updates occur
            // with the same number of MD steps in each interval, and the interval will effectively lose digits as the
            // simulation progresses, so _update_period should be cleanly representable in binary. When we extract this
            // to a facility, we can look for a part of the code with access to the current timestep.
            nextWindowUpdateTime_ += windowUpdatePeriod_;
            ++currentWindow_;

            // Reset sample bufering.
            currentSample_ = 0;
            // Clean up drift in sample times.
            nextSampleTime_ = t + samplePeriod_;
        };
    }

}

gmx::PotentialPointData EnsembleHarmonic::calculate(gmx::Vector v,
                                                    gmx::Vector v0,
                                                    double t)
{
    auto rdiff = v - v0;
    const auto Rsquared = dot(rdiff,
                              rdiff);
    const auto R = sqrt(Rsquared);


    // Compute output
    gmx::PotentialPointData output;
    // Energy not needed right now.
//    output.energy = 0;

    // Start applying force after we have sufficient historical data.
    if (windows_.size() == nWindows_)
    {
        if (R != 0) // Direction of force is ill-defined when v == v0
        {

            double dev = R;

            double f{0};

            if (dev > maxDist_)
            {
                f = k_ * (maxDist_ - dev);
            }
            else if (dev < minDist_)
            {
                f = -k_ * (minDist_ - dev);
            }
            else
            {
                double f_scal{0};

                //  for (auto element : hij){
                //      cout << "Hist element " << element << endl;
                //    }
                size_t numBins = histogram_.size();
                //cout << "number of bins " << numBins << endl;
                double x, argExp;
                double normConst = sqrt(2 * M_PI) * pow(sigma_,
                                                        3.0);

                for (auto n = 0; n < numBins; n++)
                {
                    x = n * binWidth_ - dev;
                    argExp = -0.5 * pow(x / sigma_,
                                        2.0);
                    f_scal += histogram_.at(n) * x / normConst * exp(argExp);
                }
                f = -k_ * f_scal;
            }

            output.force = f / norm(rdiff) * rdiff;
        }
    }
    return output;
}

EnsembleResourceHandle EnsembleResources::getHandle() const
{
    auto handle = EnsembleResourceHandle();
    assert(bool(reduce_));
    handle._reduce = &reduce_;
    return handle;
}

// Explicitly instantiate a definition.
template class ::plugin::RestraintModule<EnsembleRestraint>;

} // end namespace plugin