/**
 * \file CSCSegAlgoDF.cc
 *
 *  \author Dominique Fortin - UCR
 */
 
#include "CSCSegAlgoDF.h"

#include "Geometry/CSCGeometry/interface/CSCLayer.h"
// For clhep Matrix::solve
#include "DataFormats/CLHEP/interface/AlgebraicObjects.h"

#include "DataFormats/GeometryVector/interface/GlobalPoint.h"

#include "FWCore/ParameterSet/interface/ParameterSet.h"
#include "FWCore/MessageLogger/interface/MessageLogger.h"

#include "CommonTools/Statistics/interface/ChiSquaredProbability.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>


/* Constructor
 *
 */
CSCSegAlgoDF::CSCSegAlgoDF(const edm::ParameterSet& ps) : CSCSegmentAlgorithm(ps), myName("CSCSegAlgoDF") {
	
  debug                  = ps.getUntrackedParameter<bool>("CSCSegmentDebug");
  minLayersApart         = ps.getUntrackedParameter<int>("minLayersApart");
  nSigmaFromSegment      = ps.getUntrackedParameter<double>("nSigmaFromSegment");
  minHitsPerSegment      = ps.getUntrackedParameter<int>("minHitsPerSegment");
  dRPhiFineMax           = ps.getUntrackedParameter<double>("dRPhiFineMax");
  dPhiFineMax            = ps.getUntrackedParameter<double>("dPhiFineMax");
  tanThetaMax            = ps.getUntrackedParameter<double>("tanThetaMax");
  tanPhiMax              = ps.getUntrackedParameter<double>("tanPhiMax");	
}


std::vector<CSCSegment> CSCSegAlgoDF::run(const CSCChamber* aChamber, ChamberHitContainer rechits) {

  // Store chamber info in temp memory
  theChamber = aChamber; 

  return buildSegments(rechits); 
}


/* This builds segments by first creating proto-segments from at least 3 hits.
 * We intend to try all possible pairs of hits to start segment building. 'All possible' 
 * means each hit lies on different layers in the chamber.  Once a hit has been assigned 
 * to a segment, we don't consider it again, THAT IS, FOR THE FIRST PASS ONLY !
 * In fact, this is one of the possible flaw with the SK algorithms as it sometimes manages
 * to build segments with the wrong starting points.  In the DF algorithm, the endpoints
 * are tested as the best starting points in a 2nd loop.
 *
 * Also, only a certain muonsPerChamberMax maximum number of segments can be produced in the chamber
 */
std::vector<CSCSegment> CSCSegAlgoDF::buildSegments(ChamberHitContainer rechits) {

  // Clear buffer for segment vector
  std::vector<CSCSegment> segmentInChamber;
  segmentInChamber.clear();

  unsigned nHitInChamber = rechits.size();
  if ( nHitInChamber < 3 ) return segmentInChamber;

  LayerIndex layerIndex( nHitInChamber );
  
  for ( unsigned int i = 0; i < nHitInChamber; i++ ) {    
    layerIndex[i] = rechits[i]->cscDetId().layer();
  }
  
  double z1 = theChamber->layer(1)->position().z();
  double z6 = theChamber->layer(6)->position().z();
  
  if ( z1 > 0. ) {
    if ( z1 > z6 ) { 
      reverse( layerIndex.begin(), layerIndex.end() );
      reverse( rechits.begin(),    rechits.end() );
    }    
  }
  else if ( z1 < 0. ) {
    if ( z1 < z6 ) {
      reverse( layerIndex.begin(), layerIndex.end() );
      reverse( rechits.begin(),    rechits.end() );
    }    
  }

  // Initialize flags that a given hit has been allocated to a segment
  BoolContainer used_ini(rechits.size(), false);
  usedHits = used_ini;
  
  ChamberHitContainerCIt ib = rechits.begin();
  ChamberHitContainerCIt ie = rechits.end();
    
  // Now Loop over hits within the chamber to find 1st seed for segment building
  for ( ChamberHitContainerCIt i1 = ib; i1 < ie; ++i1 ) {
    if ( usedHits[i1-ib] ) continue;

    const CSCRecHit2D* h1 = *i1;
    int layer1 = layerIndex[i1-ib];
    const CSCLayer* l1 = theChamber->layer(layer1);
    GlobalPoint gp1 = l1->toGlobal(h1->localPosition());
    LocalPoint lp1 = theChamber->toLocal(gp1);  
           
    // Loop over hits backward to find 2nd seed for segment building
    for ( ChamberHitContainerCIt i2 = ie-1; i2 > ib; --i2 ) {	

      if ( usedHits[i2-ib] ) continue;   // Hit has been used already

      int layer2 = layerIndex[i2-ib];
      if ( (layer2 - layer1) < minLayersApart ) continue;

      const CSCRecHit2D* h2 = *i2;
      const CSCLayer* l2 = theChamber->layer(layer2);
      GlobalPoint gp2 = l2->toGlobal(h1->localPosition());
      LocalPoint lp2 = theChamber->toLocal(gp2);  

      // Clear proto segment so it can be (re)-filled 
      protoSegment.clear();

      // localPosition is position of hit wrt layer (so local z = 0)
      protoIntercept = h1->localPosition();

      // We want hit wrt chamber (and local z will be != 0)
      float dz = gp2.z()-gp1.z();
      protoSlope_u = (lp2.x() - lp1.x())/dz ;
      protoSlope_v = (lp2.y() - lp1.y())/dz ;    

      // Test if entrance angle is roughly pointing towards IP
      if (fabs(protoSlope_v) > tanThetaMax) continue;
      if (fabs(protoSlope_u) > tanPhiMax ) continue;
     
      protoSegment.push_back(h1);
      protoSegment.push_back(h2);
	
      // Try adding hits to proto segment
      tryAddingHitsToSegment(rechits, i1, i2); 
	
      // Check no. of hits on segment to see if segment is large enough
      bool segok = true;
      unsigned iadd = ( nHitInChamber > 20 )? iadd = 1 : 0;
      if (nHitInChamber > 30 ) iadd++;  
      if (protoSegment.size() < minHitsPerSegment+iadd) segok = false;

      if ( segok ) {

        // Fill segment properties

        // Get final slopes & chi2
        updateParameters();

        // Local direction
        double dz   = 1./sqrt(1. + protoSlope_u*protoSlope_u + protoSlope_v*protoSlope_v);
        double dx   = dz*protoSlope_u;
        double dy   = dz*protoSlope_v;
        LocalVector localDir(dx,dy,dz);
        // localDir may need sign flip to ensure it points outward from IP  
        double globalZpos    = ( theChamber->toGlobal( protoIntercept ) ).z();
        double globalZdir    = ( theChamber->toGlobal( localDir ) ).z();
        double directionSign = globalZpos * globalZdir;
        protoDirection       = (directionSign * localDir).unit();

        // Error matrix
        AlgebraicSymMatrix protoErrors = calculateError();     

        CSCSegment temp(protoSegment, protoIntercept, protoDirection, protoErrors, protoChi2); 
              
        segmentInChamber.push_back(temp); 

        if (nHitInChamber-protoSegment.size() < 3) return segmentInChamber; 
        if (segmentInChamber.size() > 4) return segmentInChamber;
        // Flag used hits
        flagHitsAsUsed(rechits);
      } 
    } 
  }
  return segmentInChamber;
}


/* Method tryAddingHitsToSegment
 *
 * Look at left over hits and try to add them to proto segment by looking how far they
 * are from the segment in terms of the hit error matrix (so how many sigmas away).
 *
 */
void CSCSegAlgoDF::tryAddingHitsToSegment( const ChamberHitContainer& rechits, 
                                           const ChamberHitContainerCIt i1, 
                                           const ChamberHitContainerCIt i2) {
  
/* Iterate over the layers with hits in the chamber
 * Skip the layers containing the segment endpoints on first pass, but then
 * try hits on layer containing the segment starting points on 2nd pass
 * if segment has >2 hits.  Once a hit is added to a layer, don't replace it 
 * until 2nd iteration
 */  
  
  ChamberHitContainerCIt ib = rechits.begin();
  ChamberHitContainerCIt ie = rechits.end();
  closeHits.clear();

  for ( ChamberHitContainerCIt i = ib; i != ie; ++i ) {

    if (i == i1 || i == i2 ) continue;   
    if ( usedHits[i-ib] ) continue;   // Don't use hits already part of a segment.

    const CSCRecHit2D* h = *i;      
    int layer = (*i)->cscDetId().layer();
    if ( isHitNearSegment( h ) ) {
      if ( !hasHitOnLayer(layer) ) {
        addHit(h, layer);  
      } else {
        closeHits.push_back(h);
      }
    }
  } 

  // Test if need to continue further
  if ( protoSegment.size() < 3 || closeHits.size() < 1) return;
  updateParameters();

  // 2nd pass to remove biases 
  for ( ChamberHitContainerCIt i = closeHits.begin() ; i != closeHits.end(); ++i ) {      
    const CSCRecHit2D* h = *i;      
    int layer = (*i)->cscDetId().layer();     
    compareProtoSegment(h, layer); 
  } 

}


/* isHitNearSegment
 *
 * Compare rechit with expected position from proto_segment
 */
bool CSCSegAlgoDF::isHitNearSegment( const CSCRecHit2D* hit) const {

  const CSCLayer* layer = theChamber->layer(hit->cscDetId().layer());

  // hit phi position in global coordinates
  GlobalPoint Hgp = layer->toGlobal(hit->localPosition());
  double Hphi = Hgp.phi();                                
  if (Hphi < 0.) Hphi += 2.*M_PI;
  LocalPoint Hlp = theChamber->toLocal(Hgp);
  double z = Hlp.z();  

  double LocalX = protoIntercept.x() + protoSlope_u * z;
  double LocalY = protoIntercept.y() + protoSlope_v * z;
  LocalPoint Slp(LocalX, LocalY, z);
  GlobalPoint Sgp = theChamber->toGlobal(Slp); 
  double Sphi = Sgp.phi();
  if (Sphi < 0.) Sphi += 2.*M_PI;
  double R = sqrt(Sgp.x()*Sgp.x() + Sgp.y()*Sgp.y());
  
  double deltaPhi = Sphi - Hphi;
  if (deltaPhi >  2.*M_PI) deltaPhi -= 2.*M_PI;
  if (deltaPhi < -2.*M_PI) deltaPhi += 2.*M_PI;
  if (deltaPhi < 0.) deltaPhi = -deltaPhi; 

  double RdeltaPhi = R * deltaPhi;

  if (RdeltaPhi < dRPhiFineMax && deltaPhi < dPhiFineMax ) return true;

  return false;
}


/* Method addHit
 *
 * Test if can add hit to proto segment. If so, try to add it.
 *
 */
bool CSCSegAlgoDF::addHit(const CSCRecHit2D* aHit, int layer) {
  
  // Return true if hit was added successfully and then parameters are updated.
  // Return false if there is already a hit on the same layer, or insert failed.
  
  bool ok = true;
  
  // Test that we are not trying to add the same hit again
  for ( ChamberHitContainer::const_iterator it = protoSegment.begin(); it != protoSegment.end(); it++ ) 
    if ( aHit == (*it)  ) return false;
  
  protoSegment.push_back(aHit);

  return ok;
}    


/* Method updateParameters
 *      
 * Perform a simple Least Square Fit on proto segment to determine slope and intercept
 *
 */   
void CSCSegAlgoDF::updateParameters() {

  // Compute slope from Least Square Fit    
  HepMatrix M(4,4,0);
  HepVector B(4,0);

  ChamberHitContainer::const_iterator ih;
  
  for (ih = protoSegment.begin(); ih != protoSegment.end(); ++ih) {
    
    const CSCRecHit2D& hit = (**ih);
    const CSCLayer* layer  = theChamber->layer(hit.cscDetId().layer());
    GlobalPoint gp         = layer->toGlobal(hit.localPosition());
    LocalPoint  lp         = theChamber->toLocal(gp); 
    
    double u = lp.x();
    double v = lp.y();
    double z = lp.z();
    
    // ptc: Covariance matrix of local errors 
    HepMatrix IC(2,2);
    IC(1,1) = hit.localPositionError().xx();
    IC(1,2) = hit.localPositionError().xy();
    IC(2,2) = hit.localPositionError().yy();
    IC(2,1) = IC(1,2); // since Cov is symmetric
    
    // ptc: Invert covariance matrix (and trap if it fails!)
    int ierr = 0;
    IC.invert(ierr); // inverts in place
    if (ierr != 0) {
      LogDebug("CSC") << "CSCSegment::fitSlopes: failed to invert covariance matrix=\n" << IC << "\n";      
    }
    
    M(1,1) += IC(1,1);
    M(1,2) += IC(1,2);
    M(1,3) += IC(1,1) * z;
    M(1,4) += IC(1,2) * z;
    B(1)   += u * IC(1,1) + v * IC(1,2);
    
    M(2,1) += IC(2,1);
    M(2,2) += IC(2,2);
    M(2,3) += IC(2,1) * z;
    M(2,4) += IC(2,2) * z;
    B(2)   += u * IC(2,1) + v * IC(2,2);
    
    M(3,1) += IC(1,1) * z;
    M(3,2) += IC(1,2) * z;
    M(3,3) += IC(1,1) * z * z;
    M(3,4) += IC(1,2) * z * z;
    B(3)   += ( u * IC(1,1) + v * IC(1,2) ) * z;
    
    M(4,1) += IC(2,1) * z;
    M(4,2) += IC(2,2) * z;
    M(4,3) += IC(2,1) * z * z;
    M(4,4) += IC(2,2) * z * z;
    B(4)   += ( u * IC(2,1) + v * IC(2,2) ) * z;
  }
  
  HepVector p = solve(M, B);
  
  // Update member variables 
  // Note that origin has local z = 0

  protoIntercept = LocalPoint(p(1), p(2), 0.);
  protoSlope_u = p(3);
  protoSlope_v = p(4);


  // Determine Chi^2 for the proto wire segment
  
  double chsq = 0.;
  
  for (ih = protoSegment.begin(); ih != protoSegment.end(); ++ih) {
    
    const CSCRecHit2D& hit = (**ih);
    const CSCLayer* layer  = theChamber->layer(hit.cscDetId().layer());
    GlobalPoint gp         = layer->toGlobal(hit.localPosition());
    LocalPoint lp          = theChamber->toLocal(gp);
    
    double u = lp.x();
    double v = lp.y();
    double z = lp.z();
    
    double du = protoIntercept.x() + protoSlope_u * z - u;
    double dv = protoIntercept.y() + protoSlope_v * z - v;
    
    HepMatrix IC(2,2);
    IC(1,1) = hit.localPositionError().xx();
    IC(1,2) = hit.localPositionError().xy();
    IC(2,2) = hit.localPositionError().yy();
    IC(2,1) = IC(1,2);
    
    // Invert covariance matrix
    int ierr = 0;
    IC.invert(ierr);
    if (ierr != 0) {
      LogDebug("CSC") << "CSCSegment::fillChiSquared: failed to invert covariance matrix=\n" << IC << "\n";      
    }
    chsq += du*du*IC(1,1) + 2.*du*dv*IC(1,2) + dv*dv*IC(2,2);
  }
  protoChi2 = chsq;
}


/* hasHitOnLayer
 *
 * Just make sure hit to be added to layer comes from different layer than those in proto segment   
 */
bool CSCSegAlgoDF::hasHitOnLayer(int layer) const {
  
  // Is there already a hit on this layer?
  for ( ChamberHitContainerCIt it = protoSegment.begin(); it != protoSegment.end(); it++ )
    if ( (*it)->cscDetId().layer() == layer ) return true;
  
  return false;
}


/* Method compareProtoSegment
 *      
 * For hit coming from the same layer of an existing hit within the proto segment
 * test if achieve better chi^2 by using this hit than the other
 *
 */ 
void CSCSegAlgoDF::compareProtoSegment(const CSCRecHit2D* h, int layer) {
  
  // Store old segment first
  double old_protoChi2                  = protoChi2;
  LocalPoint old_protoIntercept         = protoIntercept;
  float old_protoSlope_u                = protoSlope_u;
  float old_protoSlope_v                = protoSlope_v;
  LocalVector old_protoDirection        = protoDirection;
  ChamberHitContainer old_protoSegment  = protoSegment;
 

  // Try adding the hit to existing segment, and remove old one existing in same layer
  ChamberHitContainer::iterator it;
  for ( it = protoSegment.begin(); it != protoSegment.end(); ) {
    if ( (*it)->cscDetId().layer() == layer ) {
      it = protoSegment.erase(it);
    } else {
      ++it;
    }
  }
  bool ok = addHit(h, layer);

  if (ok) updateParameters();
  
  if ( (protoChi2 > old_protoChi2) || ( !ok ) ) {
    protoChi2       = old_protoChi2;
    protoIntercept  = old_protoIntercept;
    protoSlope_u    = old_protoSlope_u;
    protoSlope_v    = old_protoSlope_v;
    protoDirection  = old_protoDirection;
    protoSegment    = old_protoSegment;
  }
}


/* Method flagHitsAsUsed
 *
 * Flag hits which have entered segment building so we don't reuse them.
 * Also flag does which were very close to segment to reduce combinatorics
 */
void CSCSegAlgoDF::flagHitsAsUsed(const ChamberHitContainer& rechitsInChamber) {
  
  // Flag hits on segment as used
  ChamberHitContainerCIt ib = rechitsInChamber.begin();
  ChamberHitContainerCIt hi, iu;
  
  for ( hi = protoSegment.begin(); hi != protoSegment.end(); ++hi ) {
    for ( iu = ib; iu != rechitsInChamber.end(); ++iu ) {
      if (*hi == *iu) usedHits[iu-ib] = true;
    }
  }
//  if (closeHits.size() < 20) return;  
  // This is to deal with muon showering (not combinatorial problems)
  for ( hi = closeHits.begin(); hi != closeHits.end(); ++hi ) {
    for ( iu = ib; iu != rechitsInChamber.end(); ++iu ) {
      if (*hi == *iu) usedHits[iu-ib] = true;
    }
  }

}


/* calculateError
 *
 */
AlgebraicSymMatrix CSCSegAlgoDF::calculateError() const {

  // Blightly assume the following never fails
 
  std::vector<const CSCRecHit2D*>::const_iterator it;
  int nhits = protoSegment.size();
  int ierr; 

  AlgebraicSymMatrix weights(2*nhits, 0);
  AlgebraicMatrix A(2*nhits, 4);

  int row = 0;  
  for (it = protoSegment.begin(); it != protoSegment.end(); ++it) {
    const CSCRecHit2D& hit = (**it);
    const CSCLayer* layer = theChamber->layer(hit.cscDetId().layer());
    GlobalPoint gp = layer->toGlobal(hit.localPosition());      
    LocalPoint lp = theChamber->toLocal(gp); 
    float z = lp.z();
    ++row;
    weights(row, row)   = hit.localPositionError().xx();
    weights(row, row+1) = hit.localPositionError().xy();
    A(row, 1) = 1.;
    A(row, 3) = z;
    ++row;
    weights(row, row-1) = hit.localPositionError().xy();
    weights(row, row)   = hit.localPositionError().yy();
    A(row, 2) = 1.;
    A(row, 4) = z;
  }
  weights.invert(ierr);

  AlgebraicSymMatrix a = weights.similarityT(A);
  a.invert(ierr);
    
  // but reorder components to match what's required by TrackingRecHit interface 
  // i.e. slopes first, then positions 
    
  AlgebraicSymMatrix hold( a ); 
    
  // errors on slopes into upper left 
  a(1,1) = hold(3,3); 
  a(1,2) = hold(3,4); 
  a(2,1) = hold(4,3); 
  a(2,2) = hold(4,4); 
    
  // errors on positions into lower right 
  a(3,3) = hold(1,1); 
  a(3,4) = hold(1,2); 
  a(4,3) = hold(2,1); 
  a(4,4) = hold(2,2); 
    
  // off-diagonal elements remain unchanged 
  return a;    
} 

